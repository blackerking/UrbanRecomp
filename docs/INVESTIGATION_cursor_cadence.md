# Cursor step cadence (root-caused and tweaked)

Status: **root cause found, speed tweak applied**. The addresses in the
"What's confirmed live" section below (`$01ed`, `01:c1ca`/`01:c214`) were
never actually confirmed as the real mechanism -- they were a first
static guess later shown to be entered from `01:c3d1`, but the exact
path from there was never fully traced in that session. Superseded by
"Resolution" at the end, found via the same deterministic
`--load-state`/`--input` testing that cracked fast travel. Keeping the
original notes above it for the method-note value (the same "confirm
the actual PC, don't trust a correlated sample" lesson bit twice here
too, on a second unrelated variable this time -- see "Resolution").

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

## Resolution

Found via deterministic testing (a user-captured save state on the map
screen + `--load-state` + `--input <frame>:<duration>:<hexmask>`, holding
plain Right with zero live-keyboard jitter), plus the same `$011b`/`$011c`
write-watch infrastructure added for the fast-travel investigation
(`SC_CADENCE_WATCH`).

**Real write site**: `01:c2b1: STA $01eb` (16-bit) -- not `$01ed`/`01:c214`
as the notes above guessed. `$01eb` is the on-screen cursor's own X
position (the same byte the host-mouse patch drives), separate from
`$01bd` (map scroll-X). Confirmed by binary-searching hit counts down the
call chain (`01:8c52`→`897f`→`8985`→`899a`→`899d`→`c3cf`→`c0dd`→`c2b1`,
each stage's `SC_ADDR_TRACE` hit count over a fixed 100-frame window)
until the count dropped, which localized the gate to `01:c0dd`:

```
01:c0f0  LDA $01f3     ; 16-bit
01:c0f2  BEQ +4         ; fall through (continue toward the step) if 0
01:c0f4  DEC $01f3
01:c0f5  RTS            ; bail (no step this call) if $01f3 was nonzero
```

Confirmed directly: 15/35 calls bailed here, the other 20/35 fell through
and reached `01:c2b1`'s write, exactly matching the outer call count.
Two mirror-image sites (`01:c2df`/`c3c8`, the increment and decrement
direction handlers) both reset the counter identically after a step:
`LDA #$0003; STA $01f3`.

**Not the same bug class as the D-pad/fast-travel fixes**: this is a
plain countdown-delay constant, symmetric across both direction handlers
-- reads as deliberate pacing, not a "wrong nibble"/wrong-byte artifact.
Treated as a speed tweak rather than a bug fix: `src/main.c` patches both
`LDA #$0003` sites to `LDA #$0000`, removing the extra delay while
leaving the underlying step size (+/-2) and the outer call cadence
(itself gated by something upstream, not touched) alone. Measured
effect: the dead pause between step-bursts dropped from ~17 frames to
~5 frames in the same 100-frame deterministic test (48 `$01eb` writes
vs. 20 before). Zero regression on the full `--qualify` baseline.

If this turns out to feel *too* fast once played interactively, the
reset value (currently `0`) is a single tunable byte at both sites --
easy to dial back up (e.g. to `1`) rather than fully reverting.

**IMPORTANT -- this is only half the picture.** `docs/REVERSE_ENGINEERING_
cursor_movement.md` documents a *separate* mechanism, `$01ed` (not
`$01eb`) written via `01:c214`/`01:c1ca`, landing via `01:c221`/`01:c1d5`
-- and that investigation's "UPDATE 4" is decisive, live-bsnes-confirmed:
**real hardware steps `$01ed` every single frame while held, with zero
gaps; this recomp only does so in ~4-frame-active/~16-frame-idle bursts,
and every individual branch condition along that call chain was
independently confirmed to match real hardware's own values.** That
investigation concluded the bug is *not* a ROM branch/byte issue at all
-- it's upstream, in how often this recomp's interpreter invokes the
whole per-frame dispatcher (`01:89a0`) in the first place, i.e. a genuine
interpreter-level scheduling/cycle-accounting gap, not something a ROM
data patch can fix. My `01:c2b1`/`$01eb`/`$01f3` finding above is a
different, independently-confirmed mechanism (direct ROM-code countdown,
not a scheduling gap) -- fixing it does not address `$01ed`'s bug. If
`$01eb` and `$01ed` are the same on-screen cursor's X/Y coordinates,
**one axis may now feel noticeably snappier than the other** until
`$01ed`'s root cause (a real recomp bug, not intended pacing) is also
found. See that doc's "Where this points for a real fix" for the
concrete next step (per-frame opcode/cycle-count comparison, DMA/APU
catchup timing) -- a materially harder investigation than this one.
