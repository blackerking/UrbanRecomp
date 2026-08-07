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

**Follow-up: the full picture, decompiled.** `docs/REVERSE_ENGINEERING_
cursor_movement.md` documents a *separate* mechanism, `$01ed` (not
`$01eb`), and its "UPDATE 4" is a decisive, live-bsnes-confirmed claim:
real hardware steps `$01ed` every single frame while held, zero gaps,
while this recomp bursts. A first deterministic re-measurement (holding
plain Up, tracing only the "primary" path `01:c132`) found just 11/100
frames vs. `$01eb`'s 48/100 -- an alarming gap that looked like it
confirmed the old doc's "genuine interpreter scheduling bug, not a ROM
issue" conclusion.

**That first re-measurement was incomplete.** Both `$01eb` and `$01ed`
are gated by a *second* lock beyond `$01f3`: `$01ff`, a shared
"step-pending" flag set to a per-direction bitmask (`0x0800`=Up,
`0x0400`=Down, `0x0200`=Left, `0x0100`=Right) by each direction's step
handler, and only cleared by a shared tail at `01:c2d4`
(`AND #$0007; BEQ`) when the cursor's new position is a multiple of 8 --
roughly 1-in-4 calls, since each step moves 2 units. `01:c0fd` gates the
*entire* downstream chain (both axes) on `$01ff==0`. But there's a
**second, parallel ladder at `01:c195`** that runs when `$01ff` is
*not* clear: it re-tests whichever direction bit is still sitting in
`$01ff` and re-issues that exact step directly (`JSR $c1cb`/`$c214` for
Up/Down, `JMP $c2a1`/`$c250` for Right/Left) -- a legitimate bypass, not
dead code. Once this bypass is included, the real total is **`$01ed`:
41/100, `$01eb`: 48/100** -- much closer than 11 vs. 48, and both
governed by the identical shared mechanism. Verified by tracing `01:c195`
and `01:c1cb`/`01:c2b1` together in the same run and confirming the
counts sum correctly (`primary + bypass == total`).

**Tried removing the `$01ff` lock too** (same idea as the `$01f3` fix --
widen `01:c2d4`'s `AND #$0007` to `AND #$0000` so it always unlocks).
This is a confirmed **regression**, not an improvement: with `$01ff`
always clearing immediately, the `01:c195` bypass never finds a pending
direction to re-issue and stops contributing, and the primary path alone
doesn't make up the difference -- measured total step rate *dropped* to
25/100 for both axes. Reverted; not applied.

**Where this leaves things**: `$01eb`/`$01ed` are both real, now
well-understood, ROM-code-driven mechanisms (not a mysterious
interpreter-timing gap) -- but the remaining ~50-per-100 shortfall from
real hardware's "every frame" behavior traces back further than either
`$01f3` or `$01ff`: to the outer per-frame dispatcher's own invocation
rate (`01:c0f5`, `01:89a0`), which itself only runs ~49/100 frames.
*That* is genuinely the same question `docs/REVERSE_ENGINEERING_
cursor_movement.md`'s "UPDATE 4" pointed at -- why doesn't this whole
subsystem get invoked every frame -- and it remains open. The two fixes
applied here (`$01f3`) plus the two investigated-and-reverted attempts
(`$01ff`) narrowed the gap considerably (measured, live-confirmed
"feels a lot faster") without resolving that deeper question.

## UPDATE 6: found what's running during the "gap" frames -- bank $03, not an interpreter bug

Added a new diagnostic (`SC_FRAME_BANK_TRACE=<start>,<end>`, see the env
var docs at the top of `src/main.c`) that prints the CPU's `bank:PC` at
*every single frame boundary* in a range, unconditionally -- something
none of the existing tools could do, since `SC_ADDR_TRACE`'s PC-history
ring buffer (128 opcodes) is far too shallow to span multiple whole
frames of execution.

Pointed at a burst-then-gap cycle (frames 5368-5380), the result is
decisive and immediately obvious:

```
[framebank f=5369] k=03 pc=8ff4      <- bank 3
[framebank f=5370] k=01 pc=c038      <- bank 1 (burst: cursor dispatcher runs)
[framebank f=5371] k=01 pc=8bad
[framebank f=5372] k=01 pc=c83f
[framebank f=5373] k=01 pc=c8a5
[framebank f=5374] k=03 pc=b13a      <- bank 3 again (gap starts)
[framebank f=5375] k=03 pc=b08a
[framebank f=5376] k=03 pc=b09c
[framebank f=5377] k=03 pc=b046
[framebank f=5378] k=01 pc=c07a      <- back to bank 1 (next burst)
```

**Bank $03 -- almost certainly the city simulation tick (traffic, zone
growth, RCI demand, etc.) -- is genuinely executing during every "gap"
frame.** This isn't the interpreter silently dropping frames or an
uncharacterized scheduling gap: `nmi_requests`/`nmi_serviced` already
confirmed every single frame's NMI actually fires and gets serviced
(the qualify summary line), so the CPU is *doing real, continuous work*
the whole time -- it's just bank 3's work, not bank 1's. When NMI
interrupts bank 3 mid-pass and `RTI`s back, it resumes bank 3 exactly
where it left off, not the bank-1 main loop's `01:8951` wait-point --
i.e. the bank-1 cursor dispatcher is a **lower-priority cooperative
task that only gets a turn once bank 3 finishes its current pass**.
Reproduced cleanly across a second, independent burst/gap cycle
(frames 5393-5412) with the identical pattern.

**This changes the framing of the whole investigation.** The `$01f3`/
`$01ff`-family gates earlier in this doc are real ROM logic controlling
*how much* work bank 1 does on its turn; this is a *different* mechanism
entirely, controlling *how often* bank 1 gets a turn at all, and it
looks like authentic SimCity engine design (spreading simulation work
across multiple frames), not a bug -- the same conclusion this session
already reached for every other "mysterious timing gap" turned out to
have a concrete ROM-code explanation once actually traced, rather than
guessed at. `docs/REVERSE_ENGINEERING_cursor_movement.md`'s "UPDATE 4"
bsnes capture (showing real hardware stepping "every frame, zero gaps")
was very likely caught during a moment bank 3 had nothing queued, not a
directly comparable measurement to this session's continuous-hold test --
apples to oranges, not a confirmed recomp/hardware divergence. This is
not fully proven without a live bsnes capture of the *same* scenario
(a save state on this exact screen, direction held continuously, bank
tracked at every frame boundary) for a real side-by-side comparison --
that would be the decisive next step if this is picked up again, using
`SC_FRAME_BANK_TRACE` on the recomp side and a bsnes breakpoint that
logs the active bank on every frame on the other. Given how likely this
now looks to be authentic engine behavior rather than a bug, "make
bank 1 get more turns" is very likely the wrong thing to chase further --
it would mean fighting the game's own scheduler, with an unclear effect
on simulation timing/game balance.
