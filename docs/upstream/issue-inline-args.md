`detect_inline_arg_bytes` misses the pop/adjust/push idiom, producing wrong code

**This is a wrong-code bug, not a coverage limit.** The emitter produces
`aot_eligible` bodies that execute instructions the real CPU never executes.

Found in SimCity (SNES, USA); the idiom is general 65816, so other titles are
likely affected. A tested patch is at the bottom.

## The gap

`detect_inline_arg_bytes` (`recompiler-rs/src/decoder.rs`) recognises a callee
that consumes bytes embedded after its `JSR`/`JSL`, so the decoder can resume
the caller past them. Its doc comment scopes it deliberately:

> This deliberately recognizes only the load-return-address, add-immediate,
> store-back-to-the-same-stack-slot idiom.

and the match requires a stack-relative store:

```rust
"STA" if ins.opcode == 0x83 && ins.mode == Mode::Stk => {
    return Some((a_added & 0xFF) as u8);
}
```

SimCity expresses identical semantics two other ways — **pop the return
address, adjust it, push it back**. The stack pointer ends where it started, so
these are equivalent, just written with stack instructions instead of
stack-relative addressing:

```
; accumulator form — 03:a3cf, also 03:a2f5 / 03:a421
REP #$30 ; PLA ; TAY ; CLC ; ADC #$0003 ; PHA
...
LDA $0001,Y      ; read inline operand
LDA $0002,Y

; index form — 00:98a0 (a JSL callee)
SEP #$20 ; REP #$10 ; PLX ; PLA ; PHA ; ... ; INX ; LDA $0000,X ; INX ; PHX
```

Neither is recognised, so `inline_arg_bytes` stays 0 and the decoder reads the
inline operand bytes as code.

**Before the patch, not one row in this game's entire `dispatch_v2.c` had a
nonzero `inline_arg_bytes`**, despite five routines using the idiom at 125
call sites.

## The miscompilation

Call site `03:afd6`:

```
03:afd6  20 f5 a2     JSR $a2f5
03:afd9  06 0a 0e     <- three operand bytes, DATA
03:afdc  ad 0f 0e     LDA $0e0f     <- where the routine really resumes
```

The containing node `03:afb0-b06b` (m1x1) was **`aot_eligible` with an empty
`reasons` list** — clean, compiled, live. Its emitted body called
`bank_03_a2f5_*` and then:

```c
goto L_AFD9_M0X0;      /* implicit fall-through */
L_AFD9_M0X0:
  uint16 _v12 = cpu_read16(cpu, 0x00, (uint16)(cpu->D + 0x000a));   /* ASL $0a   */
  ...
  cpu_write16(cpu, 0x00, (uint16)(cpu->D + 0x000a), _v13);
  uint16 _v14 = cpu_read16(cpu, cpu->DB, (uint16)(0x0fad));         /* ASL $0fad */
```

It decoded `06 0a` and `0e ad 0f` as `ASL $0a` and `ASL $0fad` and emitted both
as real shifts with writebacks. The ROM executes neither, and decoding
continues from the wrong alignment thereafter.

## Proof the hardware skips those bytes

An execution bitmap recorded from real play (every executed instruction
address):

| address | executed |
|---|---|
| `03:afd6` — the `JSR` | **yes** |
| `03:afd9` — first operand byte | **no** |
| `03:afdc` — real resume point | **yes** |

## Scale in this game

| routine | bytes skipped | executed call sites |
|---|---|---|
| `00:98a0` | 2 | 69 |
| `03:a2f5` | 3 | 30 |
| `03:a421` | 3 | 17 |
| `03:a3cf` | 3 | 8 |
| `03:a350` | 3 | 1 |

125 call sites; 59 of them sit in nodes the emitter compiles, and all 59 were
miscompiled.

## Why it is silent

The operand bytes here decode to *valid* instructions (`$06`/`$0e` are `ASL`),
so nothing poisons the node and no reason is recorded. Had they decoded to
`BRK` this would have surfaced as a coverage problem instead. A differential
harness built on leaf routines will not catch it either — these callers are
not leaves.

## Patch

Extend the detector to both spellings. `x_pulled`/`x_added` track the index
form; a new `mutates_x()` invalidates it on any other write to X, mirroring the
existing `mutates_y()`.

```
"PLA" => { a_slot = None; a_pulled = true; a_added = 0; }
"PHA" if a_pulled && a_added != 0 => { return Some((a_added & 0xFF) as u8); }
"PLX" => { x_pulled = true; x_added = 0; }
"INX" if x_pulled => { x_added = x_added.wrapping_add(1); }
"PHX" if x_pulled && x_added != 0 => { return Some((x_added & 0xFF) as u8); }
```

plus carrying `a_pulled` through `TAY`/`TYA` and clearing both on the reset
arms.

The evidence is if anything stronger than the recognised idiom: a routine that
pops its own return address, adds a constant and pushes it back is
unambiguously skipping inline data — there is no benign reading.

### Result

| | before | after |
|---|---|---|
| exact variants | 1070 | **1155** |
| AOT-eligible | 722 | **845** |
| LLE-only | 348 | 310 |
| AOT share of analyzed instructions | 71.5% | **80.3%** |
| rows with nonzero `inline_arg_bytes` | **0** | 4 |

Verified per call site against the execution bitmap and the emitted labels:
**59 resume past the operands, 0 resume at a skipped byte** (66 more are in
LLE-only nodes and emit no label). All 50 existing analyzer tests still pass.

*Caution when checking this:* emitted labels are named `L_XXXX_MnXn` with no
bank component, so a global search for `L_8F0A` matches a bank-00 label when
asking about a bank-01 address. Scope the search per bank — an earlier check
of mine did not, and invented a residual bug that was not there.

## Secondary suggestion

Consider failing loudly when a `JSR`/`JSL` target is found to adjust its return
address by an amount the decoder did not account for. A wrong body that looks
clean costs far more than a rejected one.

## Reproducing

```bash
git clone --recurse-submodules https://github.com/blackerking/SimCitySNESRecomp
cd SimCitySNESRecomp
# stage your own legally obtained SimCity (USA) as simcity.sfc
PYTHON=python bash tools/regen.sh --no-tests
```

`tools/find_inline_args.py` in that repo finds the whole class empirically from
an execution bitmap rather than by pattern-matching prologues, and is
game-agnostic — pointed at another title's bitmap and ROM it will find that
game's equivalents.
