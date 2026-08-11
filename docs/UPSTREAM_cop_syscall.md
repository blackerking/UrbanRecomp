# Upstream issue draft: model `COP #$imm` as a call-with-return

For `mstan/snesrecomp`. Written against SimCity (SNES, USA), but the defect is
general: any game that uses `COP` as a syscall instruction loses most of its
static-recompilation coverage to it.

## Summary

The analyzer treats `COP` (and `BRK`) as a decode terminator:

```rust
// recompiler-rs/src/insn.rs
if insn.mnem == "BRK" || insn.mnem == "COP" {
    return false;
}
```

A function containing a `COP` is therefore truncated at that instruction and
cannot be proven AOT-eligible. In SimCity that is the single largest limit on
coverage by a wide margin — **69% of all LLE-only instructions are in
COP-implicated nodes**.

`COP` is not an exotic instruction here. SimCity uses it as its *entire syscall
mechanism*: `COP #$00` with a service number in `A`, **309 call sites**, 11
services including the LC_LZ5 decompressor and the game's own wait-for-vblank
primitive.

## What the ROM actually does

The native COP vector (`$00:FFE4`) points at `00:8211`:

```
00:8211  CLI
00:8212  PHB
00:8213  PEA $0000
00:8216  PLB
00:8217  PLB              ; DB = 0
00:8218  REP #$20
00:821a  REP #$10
00:821c  ASL A            ; service number * 2
00:821d  TAX
00:821e  JSR ($8223,X)    ; 11-entry service table
00:8221  PLB
00:8222  RTI
```

Service table at `00:8223`, with call-site counts measured over the whole ROM:

| service | handler | call sites |
|---|---|---|
| 0, 5, 6 | `00:930d` | 133 |
| 1 | `00:86a4` | 20 |
| 2 | `00:8ea9` | 45 |
| 3 | `00:8e43` | 16 |
| 4 | `00:8e75` | 19 |
| 7 | `00:9479` | 1 |
| 8 | `00:90dd` | 70 |
| 9 | `00:8f82` | 1 |
| 10 | `00:86c8` | 4 |

Service 8 is the LC_LZ5 decompressor, which independently validates the table:
every `LDA #$0008 ; COP #$00` site in the ROM is a decompression call. Service 0
is the vblank wait (`STZ $b9 ; INC $c7 ; LDA $b9 ; BEQ -6 ; RTS`), released by
the NMI handler at `00:80bc`. Both confirmed by bsnes trace, not inferred.

Critically for the analyzer: **`COP #$00` returns**. It is a `JSR` through a
table wrapped in an `RTI`, so control resumes at the instruction after the
`COP`, with the flags/DB the handler leaves. It is structurally a call, not a
terminator.

## Measurements

SimCity's manifest at 1068 exact variants / 487 roots (roots seeded from the
ROM's own dispatch tables plus entry points confirmed by recorded play
sessions):

```
total analyzed     1068 nodes, 35598 insns
  AOT-eligible      720 nodes, 25448 insns  (71.5%)
  LLE-only          348 nodes, 10150 insns
```

COP's footprint inside the LLE-only set:

```
named by a cop_at_* reason        133 nodes
containing a COP in range         145 nodes
union (COP-implicated)            159 nodes, 7017 insns
                                  = 69% of all LLE-only instructions
                                  = 20% of all analyzed instructions
```

`structural_poison` is implicated too, and this is the part that is easy to
miss. Poison is *supposed* to be width refutation — proof that a given
`(pc, m, x)` never occurs. But it is 4x enriched for COP-containing ranges
against two independent controls:

| node set | contains `COP #$00` in range |
|---|---|
| AOT-eligible (control) | 16% |
| other LLE-only (control) | 12% |
| **structurally poisoned** | **60%** |

and 210 of the 214 poisoned nodes are at addresses that **actually executed**
in a recorded play session, so they are real code, not data decoded as code.
The likely mechanism is that `insn.rs` reporting an invalid decode at the `COP`
surfaces as structural invalidity rather than as "unmodelled call".

**Upper bound if this were fixed:** AOT share of analyzed instructions would go
from 71.5% to **~91%**. That is an upper bound, not a promise — some of those
nodes would fail again for unrelated reasons once the decode continues past the
`COP`.

Supporting evidence that roots are not the limiter: the AOT share has held at
**~71% across every expansion of this game's frontier** — 314 variants, 411,
997, 1038, 1068. Seeding roots grew the total 3.4x and moved the ratio by about
one point.

## Suggested shape of a fix

The framework already has the needed concept — `dispatch_helpers` reads an
inline table at a `JSL` site and emits a static dispatch. `COP` wants the same
treatment, one level up:

1. Decode `COP #$imm` as a **call that returns**, so the decode continues at
   the following instruction instead of terminating.
2. Resolve the target where possible. For a vector-dispatch idiom like
   SimCity's, the service number is an immediate in `A` at the vast majority of
   call sites (`LDA #$000N ; COP #$00`), so the target is statically known and
   the edge can be a real demand edge to the handler.
3. Where the service number is not statically known, keep the edge unresolved
   and fall back to LLE for that call — per the existing doctrine that an
   unresolved indirect site makes the *edge* LLE, not the whole function.
4. Exit M/X should come from the handler's proven exit set, which the existing
   `callee_exit_mx` machinery already models.

A cfg directive (`cop_vector`, or reusing `hle_func` on the COP vector plus a
declared service table) would let a game state the table location without the
analyzer having to prove the vector-dispatch idiom generically.

## Reproducing

```bash
git clone --recurse-submodules https://github.com/blackerking/SimCitySNESRecomp
cd SimCitySNESRecomp
# stage your own legally obtained SimCity (USA) as simcity.sfc
PYTHON=python bash tools/regen.sh --no-tests
```

`src/gen/program_manifest.json` carries per-node `disposition` and `reasons`;
`cop_at_*` reasons name the blocking sites directly. `docs/ROM_MAP.md` has the
COP dispatcher and service table decoded.

## Related

Two other snesrecomp defects were found from this project and reported
separately: the transposed `$4218`/`$4219` auto-joypad halves, and HDMA never
executing on the interpreter tier. Unlike those, this one is not a correctness
bug — the interpreter runs `COP` fine — it is purely a static-coverage limit.
