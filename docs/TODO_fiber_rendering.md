# The fiber path renders frames the interpreter never produces

**Status: open. This is why `SC_FIBER` is opt-in rather than the default.**

## What happened

The AOT tier was briefly made the default with the fiber driving it, so the map
generator HLE at `01:f1ed` would actually run. Playing on that build, the
report was immediate and correct: widescreen defects that had already been
fixed appeared to come back. "Before the fibre we had a solid base, now it
seems to get a lot of fixed errors back."

The rendering code is identical on both paths -- nothing was reverted, and no
widescreen fix is conditional on the tier. The difference is *where the guest
runs*, and it is real.

## Reproducing it

Same save state, same inputs, frames dumped from both tiers and compared byte
for byte:

```bash
for F in 0 1; do
  SC_FIBER=$F SC_WIDESCREEN=64 SC_DUMP_DIR=out$F SC_DUMP_INTERVAL=1 \
    SC_DUMP_START=145 ./SimCitySNESRecomp --load-state savestate_0.bin \
    --qualify 158 "Sim City (U) [!].sfc"
done
```

## What the evidence says

**It is not a broken widescreen fix.** Several static frames come out
BYTE-IDENTICAL under the fiber, margins included. The pipeline is capable of
exactly right output there, so the compose, the passe-partout and the margin
fill are all running.

**It is not merely a phase shift.** That was the first hypothesis, and it is
wrong. On a static scene the interpreter renders frames 148-157 as one
unchanging image. The fiber matches it on 151, 152, 155, 156 -- and on 149,
150, 153, 154, 157 produces frames matching NO interpreter frame in the window.
The fiber is showing states the interpreter never shows, in a rough 2-on/2-off
pattern, which is what a partially-updated screen looks like.

On a scrolling scene every frame differs, most by ~0.65% of bytes and two by
~19%, across the whole width -- both margins AND the centre, so it is not
localised to the widescreen columns.

**A frame accounting difference is visible too**: `--qualify 2000` reports
`nmi_serviced=1999` under the fiber against `2000` interpreted, and
`video_changes` 2 against 3.

## ROOT CAUSE (found)

**The bridge advances the beam too.** `interp_bridge.c`'s per-opcode
`snes_sync_master_clock()` moves the same `snes->hPos/vPos` by the guest's
master delta as it executes. So when the host hands the guest a whole frame's
worth of cycles, the beam can WRAP MID-BURST -- the frame is presented from
inside the guest's update, at whatever instruction the budget happened to
expire on. That is a screen the per-opcode host never renders, because it
interleaves guest writes across the scanlines as they are drawn.

`SC_FRAME_TRACE=1` shows the cadence exactly. The guest reaches its `00:9311`
vblank wait only every FOURTH frame; on the other three the budget cuts it at
`03:83F9`. Correlating the handover PC with frame fidelity gives a clean
period-4 pattern -- every frame handed over at `009311`, and the one after it,
is byte-identical; the two cut at `03:83F9` are the novel ones.

## What has been tried

**Sizing the budget differently does not help.** Both directions are worse, and
by a lot:

| budget | frames identical (of 17) |
|---|---|
| 0.05, 0.1, 0.25, 0.5 frame | 0 |
| **1 frame** | **9** |
| 2, 4, 8 frames | 0 |

Exactly one frame is special because the host's own beam loop also advances
exactly one frame per iteration; any other value breaks step with it. The bound
is not the fix.

**Parking the beam at vblank entry before handing over** (done, in
`run_one_frame_fiber`) moved it from 7/17 to 9/17. It puts the active display
on the correct side of the guest's burst, which is the hardware order, but it
cannot stop the bridge wrapping the beam during the burst.

**Splitting the frame into two guest slices** (active-display slice, then a
vblank slice, each bounded so the beam cannot reach the next boundary) made it
strictly worse: 0/17, with master cycles nearly doubled and `nmi_requests` 387
over 300 frames. The slicing double-advances somewhere -- the guest clock is
mirrored into `g_master_cycles` per slice and the vblank detection fires twice.
Reverted. If retried, fix the accounting first.

## Where to look next

The guest must not be holding the CPU at the instant the beam crosses a
boundary. Two shapes look plausible:

1. **Let the bridge tell the host when the beam is about to wrap** and stop
   there, rather than sizing a budget in cycles and hoping. A budget is an
   open-loop guess at where the beam will be; the bridge knows exactly.

2. **Present from a snapshot** taken when the guest last parked at `00:9311`,
   rather than from live PPU state. That decouples presentation from wherever
   the guest happens to be, at the cost of a frame of latency and a buffer.

Note that the guest genuinely needs more than one frame for its work here --
it only reaches the wait every fourth frame -- so "run until the wait" is not
available as a frame boundary. Whatever is done has to be correct for a guest
that is mid-update when the frame ends, which is also what real hardware does;
the difference is that hardware scans out progressively rather than sampling
one frozen state for the whole screen.

Do NOT chase this in the widescreen code. Two independent checks say the
drawing is right: byte-identical static frames, and differences that span the
centre as much as the margins.

## What is not affected

The map generator HLE itself is verified bit-exact and is worth keeping. Under
`SC_FIBER=1` it reproduces the guest's map on all 12000 cells for three maps
across both generator branches, and in live play it produced two further maps
whose final PRNG states match values captured from the guest independently
(`2DC4/8570` for map 0, `5B19/426F` for map 2). The generator is not implicated
in any of the above.
