# Widescreen layer policies are silently dead on the legacy renderer

**FILED: https://github.com/mstan/snesrecomp/issues/25**

`origin/main` @ `fe6045c`.

## What happens

`PpuSetWidescreenLayerClamp()`, `PpuSetWidescreenLayerMirror()`,
`PpuSetWidescreenLayerRepeat()`, `PpuSetWidescreenLayerClampBand()` and
`PpuSetWidescreenLayerRepeatBand()` all store into `ppu->ws*` fields that are
read in exactly one place — `PpuWidescreenLayerExtra()` in `ppu.h`, which is
called only from `PpuWindows_Clear()` and `PpuWindows_CalcWithExtra()` in
`ppu.c`.

`ppu_draw_whole_line_legacy()` does not go through either:

```c
void ppu_draw_whole_line_legacy(Ppu *ppu, int line) {
  if (PPU_mode(ppu) == 7) ppu_prepare_mode7(ppu, line);
  int left  = -ppu->extraLeftCur;
  int right = 256 + ppu->extraRightCur;
  for (int x = left; x < right; x++)
    ppu_render_pixel(ppu, x, line);
}
```

`grep -c 'wsLayerClamp\|wsLayerMirror\|wsLayerRepeat\|PpuWidescreenLayerExtra\|PpuWindows_' runner/src/snes/ppu_legacy.c`
returns **0**. So on the legacy path every one of those setters is a no-op —
they return normally, the field is updated, and nothing reads it.

Which renderer runs is chosen by the flags passed to `PpuBeginDrawing()`:
`kPpuRenderFlags_NewRenderer` selects `PpuDrawWholeLine()`, and **0 — the value
a host gets by simply not thinking about it — selects the legacy one.**

## Measured

SimCity, `PpuSetExtraSpace(96)`, BG layers only (sprites masked off so the
count is unambiguous), counting non-black pixels in the 96-column left margin:

| renderer | clamp `0x0F` | clamp `0x00` | |
|---|---|---|---|
| legacy | 13065 | 13065 | no effect |
| new | **0** | 6956 | works as documented |

The mask does reach the PPU on both — a read-back at render time shows
`clamp=0f widenMask=00 extraL=96 extraR=96` on the legacy path too. It is
simply never consulted.

## Why it is worth a line of documentation at least

The failure mode is the bad kind: a host calls the documented API to stop its
status bar tiling into the margins, the call succeeds, and the status bar tiles
into the margins anyway. Nothing indicates which of two renderers is active or
that it matters.

Cheapest fixes, in increasing order of work:

1. Say so in the header — "the `ws*` layer policies require
   `kPpuRenderFlags_NewRenderer`".
2. Have the setters warn once when `renderFlags` lacks the new-renderer bit.
3. Teach `ppu_render_pixel()` to honour `PpuWidescreenLayerExtra()`.

For what it is worth, switching this host to the new renderer was free: at
authentic width the two are **pixel-identical** on three different screens
(title, gameplay HUD, tax menu) and the activity-qualification numbers are
unchanged.
