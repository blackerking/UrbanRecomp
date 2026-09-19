# Upstream issue draft: model `COP #$imm` as a call-with-return

For `mstan/snesrecomp`. Written against the US city-builder ROM this host targets, but the defect is
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
cannot be proven AOT-eligible. In this game that is the single largest limit on
coverage by a wide margin — **69% of all LLE-only instructions are in
COP-implicated nodes**.

`COP` is not an exotic instruction here. The game uses it as its *entire syscall
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

### The dispatcher is itself uncompilable, which compounds this

`00:8211` is not merely the target of truncated callers -- it is itself
`lle_only`, with reason `truncated_call_continuation`, because its entire body
is the indirect `JSR ($8223,X)`. So even a caller that survived its own `COP`
would land on an interpreted dispatcher. **All ~309 COP call sites reach the
interpreter regardless.** Modelling `COP` as a call and resolving the service
table statically would fix both halves at once, since the service number is an
immediate in `A` at nearly every site.

Critically for the analyzer: **`COP #$00` returns**. It is a `JSR` through a
table wrapped in an `RTI`, so control resumes at the instruction after the
`COP`, with the flags/DB the handler leaves. It is structurally a call, not a
terminator.

## Measurements

This game's manifest at 1068 exact variants / 487 roots (roots seeded from the
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
   the game's, the service number is an immediate in `A` at the vast majority of
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
git clone --recurse-submodules https://github.com/blackerking/UrbanRecomp
cd UrbanRecomp
# put your own legally obtained US ROM in the repository root (any file name)
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

### Also found: the AOT tier does not link on MSVC

Separate and much smaller, but it blocks the same goal. `cpu_state.c` ships a
fallback empty guard table:

```c
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
const RamRoutineGuard g_ram_routine_guards[] = { { 0xFFFFFFFFu, 0u, 0u } };
```

MSVC has no `__attribute__((weak))`, so the `#if` leaves a *strong* definition
that collides with the one every generated `dispatch_v2.c` emits:

```
cpu_state.obj : error LNK2005: g_ram_routine_guards already defined in dispatch_v2.obj
cpu_state.obj : error LNK2005: g_ram_routine_guard_count already defined in dispatch_v2.obj
```

`__declspec(selectany)` is the MSVC equivalent and would make the same
weak-fallback pattern work on all three compilers. This project currently works
around it with `/FORCE:MULTIPLE` on a probe target, which is not something to
ship.


### Also found: AOT and interpreter disagree on cycle counts

Third small finding, independent of the above. Running compiled bodies and the
interpreter over the same routine and the same inputs, guest state matches
every time (64/64 trials over 8 routines) but **cycle counts differ in 13 of 64
trials**, always with the compiled side counting more:

```
00:8924   AOT 760  interp 744   (+2 CPU cycles)
00:8982   AOT 760  interp 744   (+2 CPU cycles)
00:d23a   AOT 240  interp 232   (+1 CPU cycle)
```

`00:d23a` contains `LDA $d193,X` (absolute indexed, +1 only on a page cross);
the other two are branch ladders (+1 taken, +2 taken across a page). The
pattern suggests the emitted code charges data-dependent penalties
unconditionally while the interpreter charges them only when they occur —
which would explain both the direction and why only some randomised inputs
trigger it.

This matters for any host that advances devices from `cpu->master_cycles`, and
it makes a bounced-vs-interpreted differential diverge on the master clock even
when logic is bit-identical.

Reproduce with `cmake --build build --target UrbanRecompAOTDiff && ./UrbanRecompAOTDiff`.


### Also found: `indirect_dispatch` does not cover `JSR (abs,X)`

The framework already has the directive this game needs:

```
indirect_dispatch <site_pc> <count> ptrcall return:<pc> frame:<n> targets:<a,b,...>
```

It parses correctly and it is exactly the right shape — an authorised static
recovery of an indirect call, with the target list supplied by the game. But it
has no effect here, because the native analyzer only consults it for **JMP**:

```rust
// recompiler-rs/src/decoder.rs
// cfg/auto indirect_dispatch for JMP/JML indirect.
if insn.mnem == "JMP" && (insn.mode == Mode::Indir || insn.mode == Mode::IndirX) {
```

Both of the game's dispatchers are `JSR (abs,X)` — opcode `$FC`, not `$7C`:

| site | bytes | what it is |
|---|---|---|
| `00:821e` | `fc 23 82` | COP service dispatch, 11 entries at `$8223` |
| `03:d28f` | `fc 55 d2` | screen-mode dispatch, 23 entries at `$d255` |
| `01:897f` | `fc ef 88` | `$c5` reason-code dispatch |

So a `ptrcall`-mode directive — which exists precisely to describe a *call*
through a table, and even takes `return:` and `frame:` arguments for the JSR
frame — can never fire, because the only site kind that reaches the lookup is a
jump. Declaring one is silently a no-op: the cfg parses, the analysis is
unchanged, and nothing warns.

Two things would help, in order of value:

1. Extend the site test to `JSR`/`JSL` indirect (`$FC` and `$DC`). The
   `ptrcall`/`return:`/`frame:` machinery already exists and appears intended
   for exactly this.
2. Failing that, **warn on an `indirect_dispatch` directive that never matches
   a site**. A directive that silently does nothing is worse than a rejected
   one; it took a before/after manifest diff to notice.

This matters beyond this game: `JSR (abs,X)` is the standard 65816 idiom for a
call-through-jump-table, and a game that uses it for its main dispatcher cannot
currently have that edge resolved by any cfg directive.


## Fix implemented and verified

Implemented in this repo's `snesrecomp` submodule, following the same
found-here/fixed-here/offered-upstream path as the inline-argument fix.

The suggested fix above (resolve the service table statically, model COP as a
call with a proven exit) turned out to be more machinery than the problem
needs. Two observations shrink it to a handful of lines:

**1. A COP is M/X-transparent by hardware.** It pushes PB/PC/P; the handler
runs; `RTI` pops P. Whatever the handler does to the width flags — the game's
dispatcher does `REP #$20 ; REP #$10` immediately — the caller's M/X are
restored on return. So decode can continue past a COP in the entry widths
with no assumption, and no exit-M/X proof is required. This is what makes the
whole thing safe, and it is true for every game, not just this one.

**2. The right primitive already exists.** `Break(tier_to_lle=True)` lowers to

```c
/* COP: execute exact software interrupt in authoritative LLE */
return interp_tier_dispatch_tail(cpu, site, site, _entry_s, _hrv);
```

which hands the interrupt to the authoritative interpreter and unwinds to the
owning bounce rather than nesting a new one. It was only reachable when the
COP's address fell inside a declared `data_region` — the "BRK/COP-shaped
data" case. Everywhere else `_h_cop` produced a bare comment, meaning an
AOT-eligible node would have **skipped the syscall entirely**; the
`cop_at_*` poison existed to stop that from ever being emitted.

So the fix is to stop treating a COP as poison and start treating it as what
it is — a call the compiled tier cannot perform itself:

- `recompiler/v2/lowering.py` — `_h_cop` sets `tier_to_lle=True`
  unconditionally instead of gating on `data_region_exec`.
- `recompiler-rs/src/bin/analyze.rs` and `tools/v2_analyze.py` — the poison
  test and both `graph_has_poison` gates keep `BRK` and drop `COP`.

Note this is strictly safer than the status quo in the only case where the
two differ: previously an un-poisoned COP (inside a data region) already
tiered, and a poisoned one never reached codegen. There is no path that
relied on the comment.

### Result

| | before | after |
|---|---|---|
| exact variants | 1155 | **1567** |
| AOT-eligible | 845 | **1458** |
| LLE-only | 310 | 109 |
| instructions analyzed | 40800 | **66198** |
| AOT share | 80.3% | **94.9%** |

552 tail dispatches are emitted, and no `COP: software interrupt` stub
survives the regen lint.

The ~91% upper bound in this document was computed against a fixed
denominator. In practice the frontier grew by 25,398 instructions, because
truncation at a COP had been hiding everything downstream of it.

Remaining LLE-only, by instructions: `truncated_call_continuation` (2835),
`unproven_callee_exit` (2293), `brk_at_*` (638). The BRK residue is genuine —
those are wrong-width decodes, which is what the poison is for.

### Verification

- `--qualify 600` on both tiers: PASS, identical on every counter *including*
  `master=214385616`.
- All **seven** save states replayed 410 frames with scripted input, dumping
  full WRAM at frame 400: **131072/131072 bytes identical** between the
  interpreter-only and AOT builds, on every state.
- 50 analyzer tests, 81 project tests, and the regen's differential-emit
  comparison all pass.

---

## Next blocker, now that COP is gone: unpublished exit M/X

With COP modelled, the remaining LLE-only set is 3,499 instructions (5.2%),
and it has essentially one cause. Stating it precisely, because
`unproven_callee_exit` undersells it:

**300 of 1,584 nodes never get exit M/X published at all** (1,284 do), and an
unpublished exit truncates *every* caller of that node, transitively.

The clearest single example: `03:8087` is `JSR $8df1`, and `038DF1:M0X0` is
`aot_eligible` with an **empty `reasons` list** — a clean, compiled node. Its
caller is still `lle_only` with `truncated_call_continuation`, purely because
`038DF1:M0X0` has no entry in the manifest's `exit_modes`. Compilability and
exit-width publication are separate fixpoints, and the second one is what the
remaining 5% is waiting on.

The shape that defeats it is a mutually recursive dispatch cycle. Bank 01's UI
state machine is the clean case: `$01df` selects through two parallel tables
at `01:9d1a` (call) and `01:9d3a` (jump), and the five handlers
`01:A886 / A97C / AA39 / AAD5 / AD54` each dispatch through the same tables to
all five. Every one is unproven because the other four are:

```
01A886:M0X0  unproven_call_at_01A8E9_to_{01A886,01A97C,01AA39,01AAD5,01AD54}
01A97C:M0X0  unproven_call_at_01AA23_to_{ ...the same five... }
01AA39:M0X0  unproven_call_at_01AABF_to_{ ...the same five... }
```

Nothing is missing statically — the tables resolve, the targets are known, the
nodes are decoded. It is purely that the solver has no fixed point for a
cycle. The `assumptions` machinery and the comment about a "closed SCC solver"
in `v2_analyze.py` suggest this was anticipated but not finished.

`exit_mx_at <addr> <m> <x>` can assert a node's exit widths by hand, and would
break these cycles. We have deliberately not used it: it is an unchecked
assertion, and asserting a width the ROM does not actually exit in would
miscompile silently. A solver that iterates a cycle to a fixed point — or
that measures the exit widths from a recorded run and *checks* the assertion —
is the right fix.

### Corrections to earlier claims in this document

- **`indirect_dispatch` does reach `JSR (abs,X)`.** The section above says the
  directive can only fire for `JMP`. `decoder.rs` matches
  `JSR && Mode::IndirX` against `env.indirect_dispatch`, with an
  `autorecover_indirect_xtable` fallback. Verified by declaring `01:8985`
  explicitly (12 word entries at `01:88ef`): the analysis was byte-identical,
  because autorecover had already resolved it. The site truncates one step
  later, at the exit-M/X lookup.
- The related complaint that a never-matching directive is silent still
  stands, and is how the original claim went unchecked.
