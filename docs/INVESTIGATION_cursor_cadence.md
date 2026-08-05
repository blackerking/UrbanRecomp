# Cursor step cadence (investigation notes, for a future speed mod)

Status: **not yet root-caused**. These are working notes from a live
tracing session, kept here so a future session (or mod) can pick up
without repeating the same wrong turns.

## Symptom

Reported across three related but distinct spots:
1. The build cursor on the main city map moves in visible discrete
   steps ("step by step, not smooth") rather than a continuous glide.
2. The Comprehensive/Information map's own cursor is similarly slow and
   stuttery.
3. The top-menu-bar cursor moves at roughly *twice* the step speed of the
   map cursor, while navigating inside an already-open menu feels smooth/
   continuous by comparison.

Ruled out first: this is **not** a raw performance/frame-rate problem.
`SC_FRAME_TIME` instrumentation (env-gated, added to the windowed loop in
`src/main.c`) logged zero frames exceeding the 16.67ms/60fps budget across
a full play session including time spent on the Comprehensive screen. The
interpreter is not falling behind; the game's own logic is what paces the
steps.

## What's confirmed live

The visible step is a write to `$01ed` (a cursor/scroll-position byte),
via two mirror-image code blocks in bank 1, both landing on `JMP $c2d4`
afterward:
- **Decrement**, `01:c1ca-c1d7`: `LDA $01ed; DEC A; DEC A; CMP #$0018;
  BCC (skip store if clamped); STA $01ed` -- steps down by 2, clamped at
  `0x18`.
- **Increment**, `01:c214-c224`: `LDA $01ed; INC A; INC A; CMP #$00d1;
  BCC (skip store if clamped); LDA #$00d0 (clamp); STA $01ed` -- steps up
  by 2, clamped at `0xd0`/`0xd1`.

Confirmed via `SC_ADDR_TRACE`'s PC-history dump that the increment path is
actually entered from `01:c3d1` onward, **not** from `01:c1b9` as a first
static read suggested (see "Wrong turns" below). The real path, decoded
from `01:c3d1`:
```
c3d1  REP #$30 (assumed from c3cf-c3d0, 16-bit A/X/Y)
c3d1  LDA $0201            ; some 16-bit state var
c3d4  STA $79 (dp)
c3d6  LDA $01d7
c3d9  BNE +8 -> c3e3        ; ($01d7 != 0 in every observed session)
c3e3  LDA $01eb             ; -- a DIFFERENT coordinate than $01ed
c3e6  CMP #$0038
c3e9  BCC +8 -> c3f3
c3eb  LDA $01ed
c3ee  CMP #$0030
c3f1  BCS -0x18 -> c3db     ; (loops back; exact path to c214 not fully
                              traced -- PC-history buffer cut off here)
  ...
c214  [increment $01ed, described above]
```
`$01eb` and `$01ed` being compared against small thresholds (`0x38`,
`0x30`) right before the step looks like **screen-edge / scroll-follow
boundary logic** (e.g. "only advance the tracked position once the cursor
has moved far enough from center to require it"), not a simple per-frame
input-repeat timer. If that's right, a "speed mod" here isn't a single
tunable delay constant -- it's tied up with whatever scroll-vs-cursor
relationship this boundary check encodes, and needs to be understood
properly before touching it (changing the thresholds carelessly could
easily break scroll-follow behavior, not just speed it up).

## Wrong turns (so future work doesn't repeat them)

1. **First hypothesis**: `01:c1b9`'s `LDA $7c (dp); BNE ...` was assumed
   to be the entry gate selecting decrement vs. increment, with `$7c`
   read once per frame via a new `SC_DEBUG_LIVE` env var. Live sampling
   showed `$7c` cycling from `0x1d` down to `0x01` in steps of -2 every
   ~4 frames, with `$01ed` only advancing during the first few frames
   right after each wrap -- a very clean-looking "repeat delay" pattern.
   **This was a coincidence, not the mechanism**: tracing writes to `$7c`
   directly showed it's a shared direct-page scratch byte, and its
   dominant writer (`00:cdec`) is an unrelated **busy-loop delay counter
   that spins ~32 times within a single frame** (confirmed: all 32+
   writes land on the same frame number), used by some other subsystem
   entirely. The apparent frame-to-frame "countdown" was leftover scratch
   state from that loop, sampled once per frame at a fixed point --
   visually periodic, causally unrelated to `$01ed`'s cadence.
2. Confirmed the above was wrong by tracing `SC_ADDR_TRACE` directly on
   `01:c1bb` (the `BNE` right after `LDA $7c`) -- **zero hits** across an
   entire session where `$01ed` was clearly incrementing many times.
   That proved execution reaches the increment code without ever passing
   through `01:c1b9` at all, which is what led to finding the real entry
   at `01:c3d1` via PC-history.

**Lesson for next time**: don't trust a single once-per-frame sample
correlating with a static disassembly guess, even when the correlation
looks clean -- confirm the actual PC reaching the write site via
`SC_ADDR_TRACE` (with its PC-history dump) before reasoning about *why*
it fires when it does. This bit twice in the same investigation.

## Comprehensive map cursor

Separate mechanism again: this screen's cursor uses the ladder at
`02:9f37-9f43-...` (see `docs/INVESTIGATION_dpad.md`, variant 6), which
increments/decrements `$01eb,X` once per `LSR`/`BCC` pass through that
loop -- not yet cross-checked against the main map's mechanism above to
see if they share any common timing source.

## Suggested next step

Static tracing plus once-per-frame sampling has now produced two
plausible-looking but wrong theories in a row here. Given how effective it
was for the hardest D-pad bugs (Map Select, the name-entry keyboard), the
next step should probably be a **live bsnes comparison session**: set a
write-breakpoint on `$01ed` on real hardware-accurate emulation while
holding a direction, and read out the actual call stack / surrounding
values at the moment of each step, rather than continuing to reconstruct
control flow from static bytes plus scattered live samples.
