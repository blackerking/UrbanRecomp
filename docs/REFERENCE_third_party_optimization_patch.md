# Reference: third-party "SimCity Optimized" IPS patch

Status: **reference material, not applied or merged**. The user supplied
`Sim City Optimized v1_0.ips` (not tracked in this repo -- it's the user's
own local file) from an independent optimization project. This doc records
what's been learned from it so far, for future investigation -- it is
**not** a plan to merge or apply this patch; it's inspiration/cross-
reference for our own from-scratch fixes.

## What the author described (their own summary, paraphrased)

- The game's simulation logic is mostly compiled C; rendering is hand
  assembly.
- Added FastROM (incomplete -- not lookup tables or non-gameplay loading
  screens). No SA-1.
- Found several compiler-generated inefficiencies/bugs in the C code
  (redundant carry-flag comparisons, a skipped-NOP branch inside a hot
  loop, a missing `BRA` between flood/radioactive tile checks that could
  theoretically let flood act as radioactive).
- **Confirmed a two-thread cooperative model**: one "update" thread, one
  "render" thread. The render thread draws 2 rows of BG1/BG2 (+ BG3 if the
  magnifying glass/View mode is active) per activation and **only resumes
  once every 4 frames**. Once it finishes its allotted rows, it waits for
  NMI before switching back to the update thread. The author patched in
  a way to switch to the update thread immediately "under certain
  conditions" instead of always waiting.
- Map is 120x100 tiles; many redundant `x*120`/`x*60`/`x*15`
  multiplications inside loops the author replaced with running totals.
- Bern scenario benchmark: ~42.61% of original time to reach the budget
  screen at simulation speed 1; empty maps run 6-8x faster.
- Ideas not yet done by the author: DMA-based memory zeroing (needs HDMA
  sync), replacing some `MVN`/`MVP` block moves, HiROM for 64KB lookup
  tables.
- No hint of NES-derived code found; C code emits `REP`/`SEP` around
  every call/return (compiler doesn't track mode across calls), with some
  odd `REP #$20` ... `REP #$30` sequences.
- Two bugs found while rewriting electrical grid fill: (1) a stale
  `$0b89` read if the bottom-right map tile were ever a power plant
  center (not reachable without cheating); (2) power-line tiles can be
  double-counted, and the power-counter variable is 32-bit when 12000 is
  the real max -- author left this one alone.

## Why this might matter for our own bugs

**This is a very strong, independent lead for
`docs/REVERSE_ENGINEERING_cursor_movement.md`'s open cadence question.**
The described "render thread resumes once every 4 frames, then waits for
NMI to switch back" matches, almost exactly, the baseline 4-frame calling
pattern we measured all night for `01:89a0`/`01:c0dd` while idle. Their
"conditions under which it changes to the update thread immediately" is a
very plausible candidate for the exact mechanism our recomp fails to
trigger, which is why real hardware steps `$01ed` every single frame while
held and our recomp only does so in bursts.

## What's been directly confirmed by diffing the patch against our ROM

Applied the IPS to a copy of `simcity.sfc` and diffed byte-for-byte
against stock (script: see chat history / scratchpad, not checked into
this repo). 923 total byte differences across ~121 patch regions,
spanning banks 00-05.

Several patches land **exactly inside the dispatcher chain this session
already reverse-engineered** (`docs/REVERSE_ENGINEERING_cursor_movement.md`'s
`CursorMoveDispatch_Frame`/`CursorMoveDispatch_CheckModeFlags`,
`01:8b4d`-`01:8c52`ish):

- `01:8b4d`: `60` (RTS) -> `0b` (PHD) -- removes an early return, letting
  execution fall through into what we called `CursorMoveDispatch_Frame`
  instead of stopping there.
- `01:8bb2`-`01:8bb6` (5 bytes): `4c 52 8c ad 87` -> `80 22 d5 94 80`.
  The original is `JMP $8c52` (matches our documented `goto L_8c52` for
  the `$0383` mode-flag branch) followed by the start of the `$0387`
  check. The replacement is `BRA +0x22` (2 bytes) followed by 3 bytes
  that are **not executed by that BRA landing normally** -- but the BRA's
  own operand byte (`0x22`) and the following data byte can *also* serve
  as the start of a different instruction if execution reaches this
  address via a different entry point (an overlapping-instruction trick:
  landing exactly at the operand byte re-decodes it as a new opcode).
  This specific case: landing at `01:8bd7` decodes as `80 40` = `BRA
  +0x40` -> target `01:8c19`. Not fully traced further.
- `01:8bc4`: `9c` -> `80` (part of `STZ $038b` becoming part of a branch)
- `01:8c86`: `00` -> `80`
- `01:8ca6`: `c2` -> `80` (was the start of `REP #$30`)

These are clever, deliberately space-constrained micro-patches (reusing
bytes across multiple logical entry points) rather than a straightforward
rewrite -- **not safe to reverse-engineer via quick static reading alone**;
each one likely needs live tracing (in bsnes, on the patched ROM) to see
which entry points actually get used and what the full intended control
flow is. This is exactly the kind of work that benefits from live
testing help, so it's parked here rather than pursued further
autonomously.

## Suggested next step (when live-testing help is available again)

1. Load the patched ROM in bsnes, set a breakpoint on `01:8bb2` (or
   `01:8bd7`, the overlapping-entry landing spot) while holding a
   direction, and get a live trace the same way we did for stock cadence
   tracing -- see what the *intended* control flow looks like once
   optimized, and compare against our own stock-ROM trace of the same
   region.
2. Specifically look for whatever condition the author's "switch to
   update thread immediately" logic checks -- that's the single most
   valuable thing to find for our own cadence bug, since it would tell us
   exactly what our recomp needs to detect/trigger to match real
   hardware's every-frame stepping.
3. Do **not** merge or apply any of this patch's bytes directly into our
   own ROM patches -- it's a different, more invasive rewrite (FastROM,
   restructured loops) that isn't safe to partially adopt. Use it only as
   a source of understanding for writing our own, independent, minimal
   fixes the same way every other fix this session was done.
