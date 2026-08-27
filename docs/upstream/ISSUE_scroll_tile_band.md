# Map scroll: one frame in 24 shows a partially-updated tile band

**FILED: https://github.com/mstan/snesrecomp/issues/29**

Read the "What we have NOT ruled out" section before treating this as a bug:
the single most likely alternative is that this is what the real game does, and
we have no hardware or second-emulator comparison to exclude it.

*Written by an AI (Claude) working on the SimCity SNES recompilation, with the
measurements reproduced below.*

## CORRECTION (2026-08-27): this issue is wrong and should be closed

Both halves of the original report have been re-measured and neither survives.

### The "one frame in 24" band was animated water

The break frames were found by asking how well a frame matches its predecessor
translated by the scroll delta. Water in SimCity animates, so it can never
match a translated previous frame -- and the map in the repro state has a large
lake along the right side, exactly where the "band" was reported.

On the 75->76 break, of 3765 mismatching pixels **3067 (81.5%) are
water-coloured, while water is only 8.5% of the frame** -- a tenfold
enrichment. Rendering the mismatch mask makes it plainly the shoreline.

The "76.9% correctly scrolled / 63.6% stale" table in the original report is
therefore measuring the water animation, not a partially-updated tilemap. My
error, and the reason the hypothesis table looked inconclusive rather than
wrong.

### There IS a real seam, and it is the game's own design, not the runner's

Re-measured properly on the city view scrolling right:

| region | mismatch against a correctly-scrolled previous frame |
|---|---|
| middle of the screen | **0.0%** |
| leftmost 8 px | **64-88%**, once per tile column of scroll |

Isolating layers puts it entirely on BG2 (87.8%); BG1 measures 4.6%.

The mechanism is geometric. BG2's tilemap is 32x32 (`wide=0`, confirmed from
`PPU_bgTilemapWider`), so it is 256 px wide against a 256 px screen, and it
serves as a circular buffer over a city far larger than itself. Scrolling must
rewrite the column about to appear at the leading edge -- and because 32
columns wrap onto themselves, that column is *still on screen at the trailing
edge*. One column has to hold two different contents in the same frame. It
cannot, so the incoming content flashes in at the far side.

Traced per frame, the game rewrites 2 columns (~56 tilemap entries) every 8 px
of scroll, exactly when the scroll uncovers a new column. Nothing arrives late:

```
f=31 hs=52 d=+4 leftcol= 6 rightcol= 6 wrote: 6 7
f=32 hs=56 d=+4 leftcol= 7 rightcol= 6 wrote:
```

Note `leftcol == rightcol` -- the same map column showing at both edges.

Vertically there is no such problem: 32 rows is 256 px against 224 visible
lines, leaving 4 spare rows to stage into, and the trailing edge measures
1.0% or less in both directions.

### Ruled out as runner faults

| check | result |
|---|---|
| tilemap changed during active display (all BG layers, sampled every 8 lines) | **never** |
| BG2 windowed? | no -- `windowsel` nibble for BG2 is `0` |
| BG1 windowed? | yes, W2 inverted, masking x 248..255 |

The game writes `$2123 = $0c`: window BG1, leave BG2 alone. So the developers
were aware of the wrap and masked BG1's copy of it -- BG1 is offset 8 px from
BG2 (`hScroll[0] = hScroll[1] + 8`), which puts the two layers' wrap columns at
opposite screen edges. BG2's lands on the left and is left unmasked.

A plausible reason it shipped that way: on a CRT the leftmost columns sit in
overscan and were never visible. That is a hypothesis, not a measurement --
but either way the runner is reproducing what the game does, and there is no
device-model bug here to fix.

**Recommend closing.** The host works around it in the compositor, patching the
trailing sliver from the previous frame, which takes the left edge from 85.4%
to 0.0%.

### Still standing from the original report

The DMA observability note below is unaffected and still reproduces:
`ppudma_record_dma()` has no callers, so `SNESRECOMP_DMA_LOG=1` produces no
output. The colour-math note became #30.

---

*Original report follows, retained for the record.*

## Summary

While the city map scrolls, roughly one frame in 24 renders a band along the
leading edge that is **partially** updated: most tiles are correct for the new
scroll position, about a quarter are not. It reads as a vertical seam that
travels with the scroll, and at a glance looks like a tilemap wrap — content
from the opposite edge appearing.

It is not a wrap. Measured against the previous frame, the bad band matches
"correctly scrolled" better than any other hypothesis, just not well enough:

| hypothesis for the bad band | match |
|---|---|
| correctly scrolled (+2 px) | 76.9% |
| stale, not scrolled at all | 63.6% |
| wrapped from 256 px away | 0.0% |

The failing columns cluster at 8-pixel intervals, which is what you would
expect if the frame is drawn while the newly-exposed tile column is still
being written.

## Reproduction

SimCity (USA), city view, scrolling right. The game scrolls the map 2 px per
frame.

```
SC_NINTH=1 ./SimCitySNESRecomp --load-state <city.state> \
    --input 5:300:80 --qualify 130
```

`0x80` is this host's Right bit (`kPad_Up 0x10, kPad_Down 0x20, kPad_Left 0x40,
kPad_Right 0x80`).

**The save state cannot be attached** — it carries ROM-derived WRAM/VRAM, and
this project is deliberately ROM-free. To reproduce from scratch: start any
city, let the view scroll sideways continuously, and dump consecutive frames.

## Measurement

Between consecutive frames the picture should translate rigidly by the scroll
delta. Taking the best whole-pixel shift and the fraction of sampled pixels it
explains:

```
frames 60..104, scrolling right, best shift is -2 px throughout

  74 -> 75    99.9%      typical
  75 -> 76    94.1%      <-- break
  99 -> 100   83.4%      <-- break
```

Two breaks, 24 frames apart, everything else 99.5–99.9%.

On the 75→76 break, every column that fails the translation lies in x 198..248
— the right edge, where new content enters when scrolling right. The left band
(x 20..190) still matches at 99.3%.

## What it is NOT

Each of these was varied independently and produced a **byte-identical** frame:

| variation | result |
|---|---|
| recompiled tier vs AOT tier | 0 differing pixels; both break at 94.1% |
| legacy renderer vs new renderer | both break at 94.1% |
| widescreen 96 px vs authentic 256 | 0 differing pixels in the authentic columns |

So it is not code generation, not the renderer choice, and not the widescreen
path. That leaves the shared device layer (`ppu.c` / `dma.c` / `snes.c`) or the
game itself.

## What we have NOT ruled out

**That this is authentic behaviour.** We have not compared against real
hardware or another emulator. SimCity's map layer is a 32×32 tilemap — 256×256
px — against a 256×224 viewport, so vertically there are 32 spare rows to stage
into but **horizontally there are none**: every column is on screen, and a
sideways scroll must rewrite columns while they are displayed. A game doing
that has to land the write in vblank, and it is entirely possible the real
thing does not always manage it either.

That check should come first. If hardware is clean and this is not, it is a
device-model timing bug; if hardware shows the same band, this issue should be
closed.

**We could not run the reference driver on it.** `smw_cosim_ref` requires a
`SuperMarioWorldRecomp` checkout to configure, and boots from reset with no
save-state support, so it cannot be pointed at a scrolling city. The tier
comparison above is the nearest available substitute: two independent CPU
execution paths over the same devices agree exactly.

## Two incidental findings

Both encountered while investigating, neither related to the above:

- **The DMA observability ring is not wired in.** `ppu_dma_trace.h` states it
  is "compiled into EVERY build" and records every A→B DMA, but
  `ppudma_record_dma()` has no callers in `dma.c` or `common_rtl.c` in this
  checkout, so `SNESRECOMP_DMA_LOG=1` and `SNESRECOMP_PPU_HEARTBEAT` produce no
  output. Either the wiring was lost or the header overstates the coverage.

- **The new renderer mishandles halved colour math when extra space is
  non-zero.** On a screen composing backdrop plus a halved subscreen
  (`cgadsub=$60`, `cgwsel=$02`) it loses whole rectangles of the background:
  8736 of 57344 authentic pixels wrong against a 256-wide render, where the
  legacy renderer is wrong on 0. Not a geometry effect — 8, 32 and 96 px of
  extra space corrupt exactly the same 8736 pixels, so any non-zero extra
  space switches the path. Filed separately if wanted; this host currently
  works around it by selecting the legacy renderer when
  `PPU_halfColor && PPU_addSubscreen`.

## Suggested next diagnostic

With the repro above, log every VRAM write landing in the BG2 tilemap range
together with the `vPos` at which it occurs, for the frames either side of a
break. If those writes land during active display rather than vblank, the
question becomes why the game's update is arriving late; if they land in
vblank, the render is sampling the tilemap at the wrong point.
