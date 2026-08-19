# Writing a frame-model host: four traps, all silent

**FILED: https://github.com/mstan/snesrecomp/issues/22**

Not a bug report against one function so much as a report from having just built
such a host and hit every one of these. Each lets the host run, the game boot,
and only the *numbers* come out wrong — so none is caught by a headless test bar.
Filing in case the answer is "document it" rather than "change it"; happy to
send patches for whichever you think are worth it.

Context: a host that advances devices itself and drives the guest through
`interp_bridge_run_loop()`, i.e. not `RtlRunFrame`.

## 1. The beam has two owners

`snes->hPos`/`vPos` are advanced from two places at once:

| who | where |
|---|---|
| the host | its own beam loop |
| the runner | `snes_sync_master_clock()`, after **every** interpreted opcode |

Each frame ends up split between them in a ratio that varies frame to frame.
Measured: a guest-heavy frame needed ~300 host beam steps to reach the next
boundary instead of the full 178,684, because the bridge had already moved the
beam most of the way.

Anything the host derives from *its* share of the beam is then wrong in
proportion. Two consequences we hit:

**APU starvation.** Pacing the SPC off host beam steps gave ~540 samples on
host-heavy frames and **0** on guest-heavy ones — about 30% of frames — while
logic and video both looked healthy.

**Vblank entry going missing.** Sampling code (`vPos == 225 && hPos == 0`) only
fires if the host's loop is the side that crosses the line. `snes_advance_beam()`
just assigns `inVblank = v >= 225` as it sweeps, so a guest-crossed frame gets no
vblank processing at all: no `ppu_handleVblank`, no NMI, no auto-joypad arm. Cost
here was 425 NMIs where 600 were due. (`inVblank && !inNmi` detects it, since the
runner never touches `inNmi`.)

Is a host supposed to disable one of the two owners? We could not find a way to
make the runner's sync inert without patching it.

## 2. `bridge_apu_flush()` silently drops SPC time for non-`RtlRunFrame` hosts

The absolute-timeline early-out clears `s_apu_pending_master` and returns
**without advancing the SPC**, on the reasoning that `RtlRunFrame`'s absolute
clock will do the sync. A host that does not call `RtlRunFrame` therefore never
gets the guest's share of each frame delivered to the APU at all — it is not
deferred, it is dropped.

Would a guard on `rtl_apu_frame_timeline_active()` being genuinely active, or a
documented "you must call X if you take this path", be the right shape?

## 3. `snes_catchupApu()`'s 10,000-cycle clamp is a per-opcode assumption

The runaway guard is fine for a caller that catches up every opcode (~2 SPC
cycles). A frame-at-a-time host trips it every single frame: one frame is
~17,046 SPC cycles, so **41% of every frame's audio was discarded at the clamp**,
silently. Worth a comment at least — the value encodes an assumption about call
frequency that is invisible at the call site.

## 4. `interp816_opcode_hook` is a dead extension point

Declared in `interp816.h`, defined as a no-op in `interp_bridge.c`, and never
called anywhere in the runner. It reads exactly like the hook a host would use to
advance its own devices per opcode — which is what would have solved #1 cleanly.
Either wiring it up or removing it would save the next person the search.

## 5. Minor: `interp_bridge_run_interrupt()`'s contract is easy to miss

The header does say it — *"The caller has already materialized the hardware
interrupt frame."* — but calling it without one does not fail loudly. The
handler's terminal RTI pops whatever was under `S`, and the symptom is the guest
quietly ceasing to make progress (here: `logic_changes` 298 -> 0, master 173M ->
897M), which reads like a bug in the API rather than in the caller. An assert, or
a note that most hosts want `cpu_push_interrupt_frame_at()` + a resume PC instead
of this entry at all, would help.

---

Happy to send any of these as PRs. The related deadline-unwind fix is a separate
branch and is a straightforward bug rather than a design question.
