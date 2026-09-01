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

## Where to look first

The 2-on/2-off cadence and the lost NMI both point at the frame boundary
rather than at any drawing code: `SimCity_WaitForVblank` (`src/simcity_hle.c`)
and the frame driver in `src/simcity_fiberdrive.c`. The suspicion is that the
fiber hands a frame back to the host at a point where the guest has not
finished its update, so the host presents a half-written screen -- which would
also explain why the defect reads as the seam returning, since a half-composed
host map is exactly what the seam repair was built to prevent.

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
