# Upstream bug: inline-argument callees are miscompiled when the return
# address is adjusted by pop/push

For `mstan/snesrecomp`. **This is a correctness bug, not a coverage limit** —
unlike the COP report (`docs/UPSTREAM_cop_syscall.md`), which only costs AOT
coverage. Here the emitter produces `aot_eligible` bodies that execute
instructions the real CPU never executes.

## Summary

`detect_inline_arg_bytes` (`recompiler-rs/src/decoder.rs`) recognises a callee
that consumes bytes embedded after its `JSR`, so the decoder can resume the
caller past them. Its doc comment says so explicitly:

> This deliberately recognizes only the load-return-address, add-immediate,
> store-back-to-the-same-stack-slot idiom.

and the match requires a stack-relative store:

```rust
"STA" if ins.opcode == 0x83 && ins.mode == Mode::Stk => {
    return Some((a_added & 0xFF) as u8);
}
```

SimCity expresses the identical semantics the other common way — **pop the
return address, adjust it, push it back**:

```
03:a3cf  REP #$30
03:a3d1  PLA              ; pop return address
03:a3d2  TAY              ; keep it to read the operands
03:a3d3  CLC
03:a3d4  ADC #$0003       ; skip three inline bytes
03:a3d7  PHA              ; push the adjusted address back
         ...
03:a3e4  LDA $0001,Y      ; read inline operand 1
03:a3ef  LDA $0002,Y      ; read inline operand 2
```

`PLA`/`PHA` instead of `LDA n,S`/`STA n,S`. Not recognised, so
`inline_arg_bytes` stays 0 and the decoder treats the operand bytes as code.

**Not a single row in this game's entire `dispatch_v2.c` has a nonzero
`inline_arg_bytes`**, despite five routines using the idiom at 125 call sites.

## The affected routines

Found empirically rather than by pattern-matching the ROM, using
`tools/find_inline_args.py`: a recorded execution bitmap shows the call
executing, the byte immediately after it never executing, and a byte a little
further on executing. The CPU can only get there if the callee adjusted its
return address.

| routine | bytes skipped | executed call sites |
|---|---|---|
| **`00:98a0`** | **2** | **69** |
| `03:a2f5` | 3 | 30 |
| `03:a421` | 3 | 17 |
| `03:a3cf` | 3 | 8 |
| `03:a350` | 3 | 1 |

**125 call sites across five routines**, and note the largest is in bank `00`,
skipping a different number of bytes — so this is not one odd helper in one
bank. `00:98a0` is `PLX ; PLA ; PHA`, using its return address as a pointer to
two inline bytes; the bank-03 four are `PLA ; TAY ; CLC ; ADC #$0003 ; PHA`
with three operand selectors indexing the caller's direct-page frame
(`LDA $0001,Y ; AND #$00ff ; TAX ; LDA $08,X`) — a compact bytecode over bank
03's shared math layer.

The empirical approach is deliberate. The static form of the question is "does
this callee adjust its return address", which needs a decoder that recognises
every way of writing it — and getting that wrong in the conservative direction
is precisely how these were missed. An execution bitmap has no such blind
spot.

## The miscompilation, with a worked example

Call site `03:afd6`:

```
03:afd6  20 f5 a2     JSR $a2f5
03:afd9  06 0a 0e     <- three operand bytes, DATA
03:afdc  ad 0f 0e     LDA $0e0f     <- where the routine really resumes
```

The containing node `03:afb0-b06b` (m1x1) is **`aot_eligible` with an empty
`reasons` list** — clean, compiled, live. The emitted body pushes the return
frame, calls `bank_03_a2f5_*`, and then:

```c
goto L_AFD9_M0X0;      /* implicit fall-through */
L_AFD9_M0X0:
  ...
  uint16 _v12 = cpu_read16(cpu, 0x00, (uint16)(cpu->D + 0x000a));   /* ASL $0a   */
  uint16 _v13 = (uint16)((_v12 & 0xFFFF) << 1);
  cpu_write16(cpu, 0x00, (uint16)(cpu->D + 0x000a), _v13);
  uint16 _v14 = cpu_read16(cpu, cpu->DB, (uint16)(0x0fad));         /* ASL $0fad */
  uint16 _v15 = (uint16)((_v14 & 0xFFFF) << 1);
  ...
```

It decoded the operand bytes `06 0a` and `0e ad 0f` as `ASL $0a` and
`ASL $0fad`, and emits both as real shifts with real writebacks. The ROM never
executes either. Worse, decoding continues from the wrong alignment
(`$afd9 + 2 + 3 = $afde` rather than `$afdc + 3 = $afdf`), so the whole
instruction stream after the call is offset.

## Proof that the hardware skips those bytes

Not inference — an execution bitmap recorded from real play sessions
(`SC_PC_BITMAP_BANK=all`, every executed instruction address):

| address | executed |
|---|---|
| `03:afd6` — the `JSR` | **yes** |
| `03:afd9` — first operand byte | **no** |
| `03:afdc` — real resume point | **yes** |

The CPU jumps from the call straight past the operands. The emitted code does
not.

## Scale

```
call sites with proven skipped bytes : 125
  inside an AOT-eligible node        : 43
```

**43 miscompiled call sites on live paths.** Every one is a call the CPU
executed, followed by bytes the CPU never executed, inside a node the emitter
marked compilable. There is no subset where this is benign.

## Why it is silent

Two things hide it:

1. The operand bytes decode to *valid instructions* here (`06`/`0e` are `ASL`),
   so nothing poisons the node and no reason is recorded. Had they decoded to
   `BRK` the node would have been rejected and this would be a coverage
   problem instead of a correctness one.
2. A differential harness built on pure leaf routines will never catch it,
   because these callers are not leaves. This project's
   `tools/select_pure_leaves.py` excludes exactly the routines involved.

## Suggested fix

Extend `detect_inline_arg_bytes` to the pop/adjust/push form: `PLA`
(optionally `TAY`/`TAX` to retain it), `CLC`/`ADC #imm`, `PHA`. The evidence is
if anything *stronger* than the recognised idiom — a routine that pops its own
return address, adds a constant and pushes it back is unambiguously skipping
inline data; there is no benign reading.

Secondarily, consider **failing loudly** rather than silently when a `JSR`
target is later found to adjust its return address by an amount the decoder
did not account for. A wrong body that looks clean is much more expensive than
a rejected one.

## Reproducing

```bash
git clone --recurse-submodules https://github.com/blackerking/SimCitySNESRecomp
cd SimCitySNESRecomp
# stage your own legally obtained SimCity (USA) as simcity.sfc
PYTHON=python bash tools/regen.sh --no-tests
grep -n "L_AFD9_M0X0" src/gen/bank03_v2.c
```

and compare against the ROM bytes at `03:afd6`. `src/gen/dispatch_v2.c` shows
every row's `inline_arg_bytes` as the trailing field; all are `0`.

`tools/find_inline_args.py` in this repo reproduces the whole list from a
coverage bitmap, and is game-agnostic — pointed at another title's bitmap and
ROM it will find that game's equivalents.
