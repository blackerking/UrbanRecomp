# Cursor movement: named routines and pseudo-C (working notes)

This is a reverse-engineering scratchpad for the code investigated in
`docs/INVESTIGATION_cursor_cadence.md`. Addresses are `bank:addr` into
`simcity.sfc`. Confidence varies per routine -- marked inline. This is
**not** a finished map of the system; it's what's been confirmed so far,
named so future work (mine or a mod author's) doesn't have to re-decode
raw bytes from scratch.

## `01:c214` -- `StepCursorAxis_Increment` (high confidence)

```c
// M=16-bit. Entered with A already holding $01ed's old value in some
// callers (see below) -- but always re-reads it itself, so that doesn't
// matter for correctness.
void StepCursorAxis_Increment(void) {
  int16_t v = WRAM16(0x01ed);
  v += 2;
  if (v >= 0xd1) v = 0xd0;      // clamp
  WRAM16(0x01ed) = v;
  goto L_c2d4;                   // shared post-step tail, not yet decoded
}
```

## `01:c1ca` -- `StepCursorAxis_Decrement` (high confidence)

```c
void StepCursorAxis_Decrement(void) {
  int16_t v = WRAM16(0x01ed);
  v -= 2;
  if (v >= 0x18) WRAM16(0x01ed) = v;   // no clamp-and-store on underflow --
                                        // BCC skips the store entirely
  goto L_c2d4;
}
```

Both funnel into `01:c2d4` afterward (`AND #$0007; BEQ ...; JMP $c194` ->
`RTS`) -- not yet decoded in detail, looks like a small side-effect
dispatch keyed on the low 3 bits of whatever's in A at that point, not
obviously related to the cadence question.

## `01:c434` -- `CheckAxisScrollBounds` (medium-high confidence)

Called from `01:8ba3` region (see `CursorMoveDispatch_CheckModeFlags`
below) as `JSR $c434`. Reads a per-direction delta from a table and
checks whether applying it would keep both axes within bounds:

```c
// Entered with X = 0 or a direction-derived index (unclear which, see
// $01f9 below); called after: SEP #$10; REP #$20 (so A is 16-bit, X is
// 8-bit here).
void CheckAxisScrollBounds(void) {
  uint8_t dir_index = WRAM8(0x01f9);
  int16_t delta = ((int8_t)ROM8(0x0180c0 + dir_index)) << 3;  // <<3 = *8
  scratch79 = delta;

  int16_t eb = WRAM16(0x01eb) & 0xff;
  if (eb < 0x10) eb += delta;
  if (eb >= 0xf1) return;              // out of bounds -> bail (RTS)

  int16_t ed = WRAM16(0x01ed) & 0xff;
  if (ed < 0x18) ed += delta;
  if (ed >= 0xc9) return;              // out of bounds -> bail (RTS)
  // (falls through to more logic not yet decoded, then RTS)
}
```

**Uncertain**: whether this function's return value/fallthrough actually
*gates* whether `StepCursorAxis_Increment`/`_Decrement` get called
afterward, or whether it's independent bookkeeping (e.g. pre-computing
scroll clamp limits for later use). The `0x10`/`0xf1` and `0x18`/`0xc9`
thresholds are suspiciously close to `StepCursorAxis_*`'s own `0x18`/`0xd1`
clamps, which is why "scroll bounds check" is the current best guess for
this function's *purpose*, but its role in the *cadence* bug is unconfirmed.

## `01:c4c7` / `01:c4e5` -- `DirectionPriorityScan_A` / `_B` (low-medium confidence)

Two near-identical subroutines, both:
```c
void DirectionPriorityScan_X(void) {   // X = A or B, two near-copies
  uint8_t v = WRAM8(0x01f5);
  if (v & 1) { /* branch target 1 */ return; }
  v >>= 1;
  if (v & 1) { /* branch target 2 */ return; }
  v >>= 1;
  if (v & 1) { /* branch target 3 */ return; }
  v >>= 1;
  if (v & 1) { /* branch target 4 */ return; }
  return;   // all 4 bits clear -> plain RTS, observed live every time so far
}
```
In every live trace captured so far (both bsnes and this recomp), `$01f5`
was `0` at this point, so all branches fell through to plain `RTS` without
exception -- **the actual branch targets haven't been observed live and
aren't decoded yet.** `$01f5` is very likely another per-bit direction/
button state byte (possibly yet another mirror in the same
hardware-zero-nibble family documented in `docs/INVESTIGATION_dpad.md` --
not yet checked against that theory).

## `01:8b4f` -- `CursorMoveDispatch_Frame` (medium confidence)

Best-guess name for the function starting right after the previous
function's `RTS` at `01:8b4e`. Runs once per frame (confirmed: called from
the same context on every live-traced frame, active or idle alike).

```c
void CursorMoveDispatch_Frame(void) {
  // "any non-direction button held, or $0dc3 flag set" -> treat as an
  // interrupt: reset mode state and bail out to the shared tail at $8c52
  // instead of processing cursor movement this frame.
  if ((WRAM16(0x011b) & 0xfff0) || WRAM16(0x0dc3)) {
    WRAM16(0x01df) = 3;      // "screen-mode index" (see SC_DEBUG's own doc comment)
    WRAM16(0x0bcb) = 0x0a;
    goto L_8c52;
  }
  if (WRAM16(0x03fe) == 0) {
    if (WRAM16(0x0201) == 0xc0) {
      // JSL $0098a0 -- the same long call flagged in the "fast travel"
      // open item in docs/INVESTIGATION_dpad.md. Not decoded further here.
    }
    // ... more state checks not yet decoded, eventually falls into
    // CursorMoveDispatch_CheckModeFlags below.
  }
}
```

## `01:8b9f` -- `CursorMoveDispatch_CheckModeFlags` (medium confidence)

```c
void CursorMoveDispatch_CheckModeFlags(void) {
  if (WRAM16(0x0395)) goto do_thing_A;   // target not yet decoded
  if (WRAM16(0x0383)) goto do_thing_B;   // target not yet decoded
  if (WRAM16(0x0387)) goto do_thing_C;   // target not yet decoded

  // No non-direction button, or Y specifically excluded here (only tests
  // bit7=B, bit14=X):
  if (WRAM16(0x011b) & 0x4080) return;   // early-out to $8be1 in the
                                          // disassembly, i.e. this IS the
                                          // fallthrough, not a bail --
                                          // naming/control-flow direction
                                          // not fully nailed down yet

  if (WRAM8(0x01f5) == 0) {
    if (WRAM16(0x00c9) & 0x0040) {       // dp $c9, Y button (real bit)
      // ... Y-held path, not decoded
    }
  }
  if (WRAM16(0x00c9) & 0x3000) {         // dp $c9/$ca combined, L or R (real bits)
    // ... L/R-held path, not decoded
  }
  if (WRAM16(0x0201) == 0) {
    // ... eventually reaches CheckAxisScrollBounds / StepCursorAxis_*
  }
}
```

## `01:c0dd` -- `FinalCursorStepGate` (medium confidence -- confirmed reached on every burst frame, exact role still unclear)

Reached via `01:8ba2 -> ... -> 01:c3d1 (CheckAxisScrollBounds's caller) ->
01:c433 -> 01:89a0 -> 01:c0dd -> ... -> 01:c214`, confirmed via a full
128-entry PC-history dump on a live hit. This is the last function before
the actual step, so it's the strongest candidate for where the cadence
gate lives -- but **every individual check inside it has now been ruled
out live** (see below), so either the real gate is a check not yet
decoded further down this same function, or the divergence happens
*before* this function is even reached (i.e. hold-frames may take a
completely different, shorter path starting back at `01:8ba2` or earlier,
never making it anywhere near `01:c0dd`).

```c
void FinalCursorStepGate(void) {
  if (WRAM8(0x00d7) != 0) return;         // dp $d7 -- RULED OUT LIVE:
                                            // stays 0 on every sampled
                                            // frame, burst or hold alike.
  if (WRAM8(0x01f5) != 0) goto L_c38e;    // RULED OUT: $01f5 stays 0 always.
  if (WRAM8(0x01f3) == 0) {               // not yet checked live
    if (WRAM16(0x0201) != 0) {            // RULED OUT: $0201 stays 0x00ff
                                            // constant, always nonzero.
      if (WRAM8(0x01ff) == 0) {           // not yet checked live
        // reads $011b (real, already-fixed direction/A-button byte),
        // stores A-held flag to $0203, computes two per-axis "scroll
        // partner" values into dp $79/$7c from $01eb/$01ed shifted right
        // 3 (>>3, i.e. /8 -- looks like a tile-coordinate) plus $01bd/
        // $01bf, checks B/X ($011b & 0x4080), then re-enters the
        // (already D-pad-fixed) direction ladder at $011a/$0f00 before
        // eventually reaching StepCursorAxis_Increment/_Decrement.
      }
    }
  }
}
```

## Live-confirmed rule-outs (don't re-check these)

All of the following were watched frame-by-frame across a full
burst-then-hold cycle (17-frame hold, 4-frame burst, confirmed exact and
reproducible across multiple sessions) and found **constant** the entire
time -- ruled out as the cadence gate:
- `$0395`, `$0383`, `$0387` (the mode-flag checks at the very top of
  `CursorMoveDispatch_CheckModeFlags`)
- `$01f5`, `$0201`, `$0203`, `$01f9`
- `$00d7` (dp), `$01f3`... wait, `$01f3` was *not* confirmed constant --
  only `$00d7` was directly sampled and confirmed always 0. `$01f3`,
  `$01ff` were added to the watch but not yet correlated against a live
  burst/hold transition.

## What's still missing

The actual timer/counter driving the 20-frame cycle (4 active, 16 idle)
has **not been found**. Every WRAM byte checked so far along the traced
call chain into `StepCursorAxis_Increment` is either constant or doesn't
correlate with the burst/hold transition. Two live possibilities:
1. **The real counter isn't in WRAM at all** -- it could be transient,
   living only in a CPU register or on the stack within a single frame's
   execution, invisible to end-of-frame WRAM snapshots. This would need
   PC-level branch tracing (comparing full instruction traces between a
   burst frame and an adjacent hold frame), not more WRAM watching.
2. **Hold-frames never reach `01:c0dd` at all** -- the divergence could
   happen earlier, e.g. back at `01:8ba2`'s mode-flag checks or one of the
   not-yet-decoded branch targets in `CursorMoveDispatch_Frame`
   (`01:8b4f`), meaning hold-frames take a shorter, different path that
   simply never gets close to the step logic. This is actually the more
   likely explanation given how many checks along the "burst path" turned
   out to be inert (always the same value) -- a real per-frame decision
   that only sometimes proceeds this far would more plausibly live in an
   earlier, not-yet-fully-decoded branch (`01:8c00`-`01:8c52`'s several
   unresolved jump targets, `CursorMoveDispatch_Frame`'s `$03fe`/fast-travel
   checks, etc.) than in a function where every checked condition is a
   constant.

## UPDATE: exact divergence point found -- `01:c156`

Bisected via simultaneous `SC_ADDR_TRACE` on 9 waypoints between `c0dd`
and `c214` in one session (200-frame window): `01:c0dd` fires on
essentially every frame (190/200); `01:c132` (ladder entry) only 46/190;
`01:c13d` and `01:c155` both 24/46 (moving together, no further loss
between them); then **`01:c15f` drops to 7/24** -- the steepest cliff in
the whole chain. Everything from `01:c15f` through `01:c212` moves
together at 7/7 after that.

Careful byte-exact re-disassembly (an earlier pass had a one-byte
misalignment right at this spot -- a `BEQ` at `01:c138` had been missed
entirely) of `01:c132`-`01:c15f`:

```c
// M=16-bit throughout.
LDA $011a          // c132 -- patched site (was $011b); word = $011b<<8|$011a
AND #$0f00         // c135 -- bits8-11 = $011b's real direction nibble:
                     //   bit8=Right, bit9=Left, bit10=Down, bit11=Up
BEQ c194 (RTS)      // c138 -- bail if literally no direction held
ASL A               // c13a  (bit8->9->...: after 5 ASLs, carry = orig bit11 = Up)
ASL A               // c13b
ASL A               // c13c
ASL A               // c13d
ASL A               // c13e
BCC c155            // c13f -- branch if bit11 (Up) clear
  // (Up-held path, not decoded -- not reached with Right held)
c155:
ASL A               // c155 -- 6th shift; carry now = orig bit10 = Down
BCC c16c             // c156 -- ** THE DIVERGENCE POINT **
  PHA                 // c158 (bit10/Down WAS set -- not our case, not decoded)
  LDA #$0400
  STA $01ff
  JSR $c1f3           // c15f -- taken 7/24 times
```

**The puzzle**: with Right held steady, this is testing bit10 (Down) and
bit11 (Up) -- bits that should be *constant zero* every single frame,
since the player isn't pressing Up or Down at all. A steady, always-zero
bit can't explain a branch that's sometimes taken and sometimes not. Two
live possibilities:
1. **A is contaminated before reaching here.** Something between `c132`'s
   fresh `LDA $011a` and `c156` corrupts bits 10-11 of A with unrelated
   data (there's a `PHA` at `c13f`'s not-taken path and other pushes
   nearby -- if a stack push/pop pairing is subtly wrong on some call
   path, a leftover stack value could bleed into what should be a clean
   shifted copy of the direction word). Not yet confirmed.
2. **This ladder was never really about testing 4 independent direction
   bits in the way `docs/INVESTIGATION_dpad.md`'s other ladders do** --
   it might intentionally combine bits (e.g. testing diagonal or
   axis-priority combinations), and the "7/24" pattern is a real,
   ROM-accurate consequence of *some other, still-unidentified byte*
   changing bits 10-11 through legitimate game state (e.g. a scroll/
   cursor-position-derived value being ORed in before this test, not a
   corruption at all).

## UPDATE 2: confirmed live -- the gating bit is stale/reused state, not fresh input

Pulled the `a=` value from every live `01:c155` hit in the same session
and correlated it frame-by-frame against `01:c214`'s hits. Exact match:
every burst of `StepCursorAxis_Increment` calls starts on precisely the
frame where `01:c155`'s accumulator has **bit 15 set** (`a=0x8000`); every
`01:c155` hit with a different bit set (`0x4000`, `0x2000`, ...) produces
*zero* `c214` calls until the value changes back to `0x8000`.

```
f=6381..6521  (8 samples, 20 frames apart): a=0x4000 -- no c214 hits
f=6541..6661  (7 samples, 20 frames apart): a=0x8000 -- c214 fires x4 every single time
f=6681..6841  (9 samples, 20 frames apart): a=0x2000 -- no c214 hits
```

This rules out "fresh per-frame input read, sometimes true sometimes
false" entirely: bits 13/14/15 correspond (after the 6 `ASL`s applied
since `c132`'s `AND #$0f00`) to the *original* bits 7/8/9 of the
combined word -- i.e. neither a real direction bit nor B/Y/Select cleanly on
their own (bit 9 doesn't correspond to any button in the established
mapping). The value held **constant for 140-160 frames at a stretch**
(7-9 samples in a row, each 20 frames apart) before jumping to a
completely different single bit is not consistent with re-reading `$011a`
each frame either -- `$011a` reflects live held-button state and would
either be constant the *entire* time direction is held (it doesn't) or
change on human input timescales, not suspiciously-clean ~20-frame-aligned
cycles matching the very cadence bug being investigated.

**Best current theory**: this specific branch at `01:c156` isn't testing
this frame's real input at all -- `A` most likely still holds leftover
data from something else executed between `c132`'s load and here (the
`PHA`/pop pairs on the not-taken side of nearby branches are the likely
suspects, but not confirmed), or the whole `c132`-`c156` ladder is a
**shared, multi-purpose dispatch** stepping through several different
"which object gets updated this call" slots (of which the player's cursor
is only one), cycling once every ~20 frames -- which would mean the
cursor's *real* movement instruction only actually gets *reached* on its
turn in a fixed rotation, independent of whether a direction key is held
that frame at all. If that's right, the fix isn't a byte patch at
`c155`/`c156` -- it's understanding and adjusting whatever assigns "turns"
in that rotation, which hasn't been located yet.

**Concrete next step**: trace writes to `A` (i.e. every `LDA`/register
snapshot) between `c132` and `c155` across one call, to see exactly which
instruction sets the value that ends up being tested at `c156` -- the
current trace only samples `A` at `c155` itself, one instruction too late
to see where that value actually came from.

## UPDATE 3: live bsnes comparison confirms the *path* is correct -- the *call frequency* is the real bug

User captured two live bsnes traces of the real ROM for direct comparison:
1. A breakpoint hit at frame ~21 (before the title screen even settles,
   `$011b=0`, `$0201=0`) showed execution take a completely different,
   simpler branch (`01c0fa: jmp $c2e6` -> `01c2f3: LDA $011b; AND #$0f00;
   BEQ` -- no ASL ladder at all). This briefly looked like a smoking gun
   (a "simpler path exists that we never take"), but it turned out to be
   unrepresentative boot-time state, not real gameplay -- ruled out once a
   proper in-gameplay trace was captured.
2. A **real, in-gameplay** breakpoint hit on `$01ed`'s write (`01:c221`,
   `A:0082`, matching our own recomp's values exactly) showed the *exact
   same* call chain we'd already mapped: `01:c221` -> `01:c2d4` -> `RTS` ->
   `01:c162` (the ASL-ladder direction-priority dispatcher) -> `01:c4c7`/
   `01:c4e5` (`DirectionPriorityScan_A`/`_B`, both confirmed idle, `$01f5`
   always 0, matching our own traces) -> `RTS` -> **`0189a3: JSR $c434`**
   (`CheckAxisScrollBounds`) -> `RTS` -> **`0189a6: JSR $c616`** -> ... ->
   **`0189a9: JSR $8a92`** -> **`0189ac: JSR $948b`** -> **`0189af: JSL
   $008432`** -> **`0189b3: JSL $0094d5`** -> (continues into further
   subsystems: sprite-table updates, a `$0b4b`-indexed table-driven update
   loop at `$94fd`-`$95d8`, etc.)

This is decisive: **real hardware takes the identical code path** our
recomp does for actual cursor movement -- the earlier "different branch"
theory is dead. But it revealed something more useful: this whole
`0189a0` -> `0189a3` -> `0189a6` -> `0189a9` -> ... dispatcher chain
completed *within a single NTSC frame* on real hardware (`V:243...`
through the whole sequence, crossing exactly one `F:47`->`F:48` frame
boundary) -- i.e. **this entire per-frame dispatcher runs every single
frame, unconditionally, on real hardware.**

Compare to our own recomp's traces of the same entry points
(`01:89a0`, `01:c0dd`): hit consistently, but **not every frame** -- the
pattern across every session was "mostly every 4 frames, with periodic
bursts of 3-4 consecutive-frame hits" (see the frame-number dumps earlier
in this doc). That burst-then-4-frame-gap shape is the signature of a
scheduler that's supposed to run something every frame but sometimes
skips and later catches up -- and it now looks like **this happens to the
whole `0189a0` dispatcher itself**, not to any single branch inside it.

**Revised understanding**: the bug is very likely not a wrong branch
condition anywhere in `c132`-`c214` (every individual check inside that
range has been live-verified to behave identically to real hardware's
values). It's that **our recomp doesn't invoke the `0189a0` dispatcher as
consistently as real hardware does** -- something upstream of `0189a0`
(a cooperative task scheduler in the ROM, or possibly a genuine emulation
timing gap in this interpreter -- NMI servicing, opcode-count-per-frame
budget, DMA/audio catchup stealing cycles) causes this call to be skipped
some frames and bundled into catch-up bursts on others.

**Not yet done**: trace what calls `0189a0` itself, and compare *that*
caller's own frequency against real hardware (the same live-bsnes
technique that worked here, one level further up the stack) -- this is
the next concrete step, and is a different kind of question than anything
chased in this document so far (a scheduling/timing question, not a
branch-logic question).

## UPDATE 5: found a likely task-scheduler jump table (autonomous session, static-only, unverified)

A collaborator independently optimizing this ROM described (see
`docs/REFERENCE_third_party_optimization_patch.md`) a cooperative
update/render two-thread model where "the render thread only resumes once
every 4 frames" -- matching this document's measured baseline exactly.
They also describe patching in a way to switch threads immediately
"under certain conditions" instead of always waiting, which is very
plausibly the exact mechanism this recomp fails to trigger.

Searching the ROM for the raw byte pattern of `01:8b4e`'s address (the
`CursorMoveDispatch_Frame` entry point -- never called via a direct
`JSR`/`JSL` anywhere in the ROM, ruling out a fixed call site) found it
appearing exactly once, as apparent table data at **bank `0d`, address
`~0xe07a`**, alongside several other addresses that also fall inside
bank 01's `0x8xxx` code range (`0x8b51`, `0x8c44`, ...). This is a strong
candidate for the task-scheduler dispatch table itself.

**Not yet confirmed**: no direct long-addressing (`JSL`/`LDA long`)
reference to this table region was found via static byte search either --
it's likely accessed via absolute addressing with the data bank register
separately set to `$0D` beforehand (the same `PLB`-from-a-loaded-byte
pattern seen in the decompressor at `00:90dd`), which a simple byte-level
search can't locate without knowing where that `PLB` happens. This needs
either live tracing (set a read-breakpoint on `$0d:e07a` and see what
reads it and with what index) or a much broader static search for `PLB`
sites feeding data-bank `$0D`. Genuinely the most promising open lead for
the cadence bug, but real verification requires live testing help.

## UPDATE 4: CONFIRMED -- real hardware steps every single frame while held; this is a genuine recomp bug

Two decisive pieces of live data settle this:

1. **Idle (no direction held)**: traced `01:89a0`'s own calling frequency
   in this recomp with no input -- a perfectly clean, consistent 4-frame
   period (5488, 5490, 5494, 5498, 5502, ...), no bursts. This alone isn't
   a bug signature; it's consistent with a legitimate cooperative
   scheduler giving this subsystem one "turn" every 4 frames when there's
   nothing to do.

2. **Actively held (real hardware, live bsnes breakpoint on the `$01ed`
   write, user continuing past 6 consecutive hits)**:
   ```
   hit#6: F=49  A=0086  (01:c221, increment path)
   hit#7: F=50  A=0088  (01:c221, increment path)   <- consecutive frame
   hit#8: F=5   A=0086  (01:c1d5, decrement path)
   hit#9: F=6   A=0084  (01:c1d5, decrement path)   <- consecutive frame
   hit#10: F=7  A=0082  (01:c1d5, decrement path)   <- consecutive frame
   hit#11: F=8  A=0080  (01:c1d5, decrement path)   <- consecutive frame
   ```
   Every single hit lands on the *immediately following* frame. **Real
   hardware steps the cursor by 2 every single frame while a direction is
   held, continuously, with zero gaps.** No burst-then-pause pattern at
   all.

This conclusively rules out "authentic ROM-accurate slow cadence" as an
explanation. It also rules out every branch-condition theory chased
earlier in this document (`$7c`, `$00d7`, `$01f5`, `$0201`, the `c156`
bit-15 test, etc.) as *root causes* -- every one of those was independently
confirmed to evaluate identically to real hardware's own values when
sampled. The only remaining explanation is that **our interpreter simply
doesn't invoke the whole `0189a0` per-frame dispatcher as often as real
hardware does while this particular game state is active** -- i.e. this is
a scheduling/timing gap in the interpreter or in how per-frame CPU budget
is being consumed, not a mis-decoded branch anywhere in the traced ROM
code itself.

### Where this points for a real fix

Every branch inside `01:8ba2`-`01:c214` has now been individually
confirmed correct against real hardware. The bug is almost certainly
*upstream* of `01:89a0` -- in whatever decides how many "task slots" run
per frame, or in a genuine per-frame instruction/cycle-count mismatch
between this interpreter and real 65816 timing that causes some
scheduled work to be silently skipped and caught up later. This needs a
different kind of investigation than anything done in this document:
- Compare total opcodes/cycles executed per frame between this recomp and
  a cycle-accurate reference for a frame during held-direction movement.
- Check whether DMA/HDMA catchup (`dma_cycle`, and the newly-added
  per-line HDMA execution from `docs/INVESTIGATION_hdma.md`) or audio
  APU catchup (`snes_catchupApu`) could be consuming cycles unevenly
  frame-to-frame in a way the ROM's own scheduler reacts to.
- Find and trace the ROM's own scheduler/dispatcher one level above
  `01:89a0` (what decides that this is one of N tasks and how often each
  gets a turn) and compare its real-hardware cadence against this
  recomp's.
