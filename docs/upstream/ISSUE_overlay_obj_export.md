# Overlay capture: OBJ is removed from the game render but never exported

**FILED: https://github.com/mstan/snesrecomp/issues/31**

> **Written by an AI.** This was researched, measured and written by Claude
> (Anthropic), working on a SimCity SNES recomp host with @blackerking, who has
> reviewed and approved posting it. Every number below is from an actual run
> against this repo, but the reasoning is mine and worth sanity-checking.

## What happens

With an overlay surface bound and a capture armed for both `Bg3` and `Obj`
using `kPpuOverlayFlag_RemoveFromGame`:

* **BG3 works exactly.** The surface comes back with precisely the pixels BG3
  drew.
* **OBJ does not.** The surface stays empty on every frame, while the sprites
  are still removed from the game's own render — so they are taken out of the
  picture and never handed back.

The asymmetry is the point: the same call, the same flag, one source works and
the other does not.

## Measurement

City view, one frame, 448×224 (extra space 96 per side), BG mode 1:

| source | pixels the layer renders | pixels the export surface reports |
|---|---|---|
| BG3 | 15378 | **15378** |
| OBJ | 6262 | **0** |

The BG3 figure matches to the pixel, which is what makes this specific rather
than "the export is broken". The rendered counts are from isolated renders of
each layer over the same frame.

Stable across frames:

```
host map: composing, bgmode=1 bg3px=0     objpx=0     <- first compose, before arming
host map: composing, bgmode=1 bg3px=15378 objpx=0
host map: composing, bgmode=1 bg3px=15378 objpx=0
... identical for every subsequent frame
```

Note the first line. A host that samples only the first compose sees
`bg3px=0 objpx=0` and concludes the whole export is dead — which is what
happened here for a long time, and is worth guarding against in any example
code.

## The removal side does work

Arming the capture at all changes the guest's own picture, which is the
`RemoveFromGame` flag behaving as documented:

| | authentic 256 columns |
|---|---|
| capture not armed | reference |
| capture armed | **42737 pixels differ** |

For example authentic (60,8) — the status bar's dark background — goes from
`(49,16,0)` to the map colour beneath it, because BG3 has been lifted out.

So for OBJ the net effect is destructive: sprites leave the game render and do
not arrive in the surface. Any host arming an OBJ capture loses them.

## Why it matters

This is the mechanism a host would use for per-layer effects — capture a layer,
process it, composite it back. It works for backgrounds. For sprites the layer
can currently only be deleted, not borrowed.

## Reproduction

```c
PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg3, bg3_surface, pitch);
PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj, obj_surface, pitch);
/* per frame, before any line renders: */
PpuClearOverlayCaptures(ppu);
PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg3, 0, 0, w, h,
                     kPpuOverlayFlag_RemoveFromGame);
PpuSetOverlayCapture(ppu, kPpuOverlaySource_Obj, 0, 0, w, h,
                     kPpuOverlayFlag_RemoveFromGame);
```

Then count pixels with a non-zero alpha byte in each surface after the frame.
BG3 is populated; OBJ is empty. Both layers disappear from the game's own
output.

Observed with `kPpuRenderFlags_NewRenderer` set, BG mode 1, extra space 96.
I have not checked whether it also affects mode 0 or authentic width, nor
whether a capture without `RemoveFromGame` behaves differently.

The save state that shows it cannot be attached — it carries ROM-derived WRAM
and VRAM, and that project is deliberately ROM-free. Happy to run an
instrumented build against it if that is more useful than a synthetic case.
