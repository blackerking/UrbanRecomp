# Step 3b: what routing execution through compiled bodies actually requires

Steps 1–3a are done: the generated banks build, link, share a binary and a WRAM
array with the interpreter, and 8 compiled bodies have been differentially
verified against the real ROM routines (64/64 comparisons identical). This
records what stands between that and actually *running* compiled code, because
it is more than wiring and should be understood before anyone starts.

## 1. The two tiers have incompatible timing models

This host's frame loop is **per-opcode and dot-accurate**:

```c
int cyc = interp816_runOpcode(cpu);
int master = cyc * 8;
for (int i = 0; i < master; i += 2) handle_pos_stuff();   /* PPU H/V position */
snes->apuCatchupCycles += (double)master * kApuCyclesPerMaster;
snes_catchupApu(snes);
```

Every opcode advances the PPU dot by dot. That model is why this project could
find and fix the missing-HDMA bug, and it is what the `--qualify` baseline
rests on.

The AOT runtime's model is **per-frame**: `RtlRunFrame` (`common_rtl.c:421`)
calls `g_rtl_game_info->run_frame()`, a per-game function that runs a whole
frame of guest code, and the PPU is advanced around it rather than inside it.
`interp_bridge` tracks `cpu->master_cycles` and does APU catch-up itself
(including a batching optimisation that collapsed interp-heavy frames ~250x)
but does **not** step the PPU per opcode.

So a compiled body runs many opcodes with no dot-level device advance. Three
ways out, in increasing order of disruption:

1. **Advance devices by the body's cycle delta on return.** `CpuState` carries
   `master_cycles`, so after a compiled call the host can run
   `handle_pos_stuff()` for the elapsed master cycles. Total timing stays
   right; intra-routine dot accuracy is lost. Fine for pure computation,
   wrong for anything that touches PPU registers mid-scanline.
2. **Only bounce into bodies proven not to touch hardware** — the same
   pure-leaf property `tools/select_pure_leaves.py` already computes. Safe,
   but limits the win to exactly the routines that matter least.
3. **Adopt the runner's frame model** wholesale. Most faithful to the
   framework, and the largest change: it replaces the loop this project's
   accuracy story is built on, and re-validating it means redoing the HDMA and
   audio work that got `--qualify` passing.

None of these is obviously right, which is the point of writing it down.

## 2. What it would buy, measured

Of twelve routines identified as hot or structurally important:

| routine | tier |
|---|---|
| `00:824f` PRNG step | **AOT** |
| `00:90dd` LC_LZ5 decompressor | **AOT** |
| `00:930d` vblank wait | **AOT** |
| `00:929b` edge detector | **AOT** |
| `03:d15f` map unpacker | **AOT** |
| `01:f1f1` terrain generator | **AOT** |
| `03:a390` 32-bit multiply | LLE |
| `00:8211` COP dispatcher | LLE |
| `03:b152` power scan | LLE |
| `03:ce2e` scenario map loader | LLE |
| `03:ddb6` scenario select | LLE |
| `03:e2ee` win-mark setter | LLE |

Six and six. The misses are not random — each has a specific cause, and two of
them matter a lot.

### The COP dispatcher is itself LLE, which compounds

`00:8211` fails with `truncated_call_continuation`, because its whole body is
`JSR ($8223,X)` — an indirect call through the service table. So it is not just
that COP truncates every *caller* (the finding in
`docs/UPSTREAM_cop_syscall.md`): the dispatcher those callers reach is
uncompilable too, so **every one of the ~309 COP call sites lands on the
interpreter no matter what**. `03:ce2e`, the scenario map loader, shows the
caller half of the same problem directly: `cop_at_03CE5C`, `structural_poison`.

### One unproven callee exit blocks 7,643 bytes

`03:a390`, the 32-bit software multiply and the simulation's arithmetic
workhorse, sits inside a single node spanning `03:90c5`–`03:aea0` — 7,643 bytes
— that is LLE for:

```
unproven_call_at_03ABF2_to_03AC5B_m0x0
truncated_call_continuation
unproven_callee_exit
```

The blocking site is tiny:

```
03:abf0  BNE +3
03:abf2  JSR $ac5b      <- this call
03:abf5  RTS
```

and the callee begins `REP #$20 ; PHD ; TDC ; SEC ; SBC #$0008 ; TCD` — it
allocates an 8-byte direct-page frame. The analyzer cannot prove which (m, x)
it returns with, and that one unproven exit costs the whole 7.6KB node.

**Measured** (`SC_ADDR_TRACE=03:abf5`, which reports live M/X at the
instruction after the call returns): every observed return is **m16 x16**.

That is *not* offered as a fix. It is 7 samples from a single save state —
states 1 and 3 never reach the call at all — and a routine can exit at one
width on the path you sampled and another on a path you did not. Declaring
`callee_exit_mx` on that evidence would change codegen on the strength of an
under-sampled guess, which is worse than leaving it interpreted. It is recorded
because it is a concrete, reproducible data point for the upstream exit-mode
inference: `LLE_FIRST_ANALYSIS.md` says exit-mode **sets** are implemented, so
a case where inference still fails is worth someone's attention.

### `03:b152` is a genuine width refutation, not a defect

The power scan's `m1x1` variant fails with `brk_at_03B168` + `structural_poison`,
and that is the analyzer working correctly. The routine's prologue contains
`SBC #$0008` — an m-dependent immediate. Decoded with 8-bit A, `e9 08 00`
becomes `SBC #$08` followed by `00` = `BRK`. The poison is proof that the
`m1x1` entry never really happens, exactly as the poison-driven width
refutation in `LLE_FIRST_ANALYSIS.md` describes.

## 3. Order of work, if resumed

1. Decide the timing model (§1). Nothing else matters until that is settled.
2. Drive one frame through `interp_bridge_run_scheduler(cpu, entry, 0x009313,
   0x00b9)` — SimCity's yield primitive is `00:930d` spinning on `$b9`, and it
   is a plain `RTS`-returning primitive, so it should not need MMX's
   coroutine-switch handling. Note the **auto-quiescent** variant is wrong for
   this game: the spin does `INC $c7` every iteration, so the state is not
   read-only and the quiescence detector will never fire.
3. Run the differential gate from `LLE_SCHEDULER.md`: bounced vs interpreted
   must be bit-exact over the attract demo. This project already has the tools
   for it — `--qualify` hashes logic/video/audio per frame, and
   `SC_PC_BITMAP_BANK=all` shows which paths each side took.
4. Only then consider whether the COP and exit-mode limits are worth chasing
   upstream first; with a third of executed code having no body to bounce into,
   the measured win may be small enough to change the plan.
