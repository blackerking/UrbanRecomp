# Widescreen support: implementation plan (not yet implemented)

Status: **scoped, not implemented**. This turned out to be far more
tractable than initially assumed -- the shared `snesrecomp` engine already
has a complete, mature widescreen rendering system
(`snesrecomp/runner/src/snes/ppu.h`/`.c`, the `PpuSetWidescreen*`/
`PpuSetExtraSpace*` family), already used by other games built on this
framework (the header cites "the snesrec/zelda3 model" and an SMW-specific
BG3 water example). This is **not** a from-scratch PPU feature to build --
it's a configuration + game-specific glue task in `src/main.c`, similar in
spirit to the D-pad/HDMA fixes but for a new feature instead of a bug.

Deliberately not implemented in this pass: getting the margin/layer
configuration wrong would show as visible rendering glitches (tearing,
wrong-layer bleed, HUD elements stretching into margins) that can't be
caught without actually looking at the running game, which wasn't
available this session.

## What the engine already provides (read from `ppu.h`, not yet tested)

- `PpuSetExtraSpace(ppu, extra)` / `PpuSetExtraSpaceCentered(ppu, budget)`:
  establish a symmetric per-side pixel budget; render width becomes
  `256 + 2*extra`.
- `PpuSetExtraSideSpace(ppu, left, right, bottom)`: asymmetric margin,
  re-applied every frame (for games whose own scroll state should drive
  the margin dynamically -- probably not needed for this game, which likely
  wants a fixed border).
- `PpuSetWidescreenLayerMask(ppu, bg_layer_mask)`: restrict *which* BG
  layers are allowed to render into the side margins at all.
- `PpuSetWidescreenLayerClamp(ppu, mask)`: per-layer opt-out, keeping bit
  L's layer authentically 256-wide even when others extend (this is very
  likely what the game wants for its toolbar/HUD layer).
- `PpuSetWidescreenLayerMirror`/`PpuSetWidescreenLayerRepeat`: fill the
  new margin columns by reflecting or repeating the authentic edge
  scanline, for layers that don't have real off-screen tile data to show
  (may or may not apply to the game's map -- see open question below).
- `PpuSetWidescreenHudSplit`/`PpuSetWidescreenBg3Widen`: finer-grained
  HUD-specific controls, probably not needed for a first pass.

## What's needed game-side (`src/main.c`)

1. Increase `kVideoWidth` (currently `256`, `src/main.c:201`) to
   `256 + 2*extra` for the chosen border size, and update the SDL window/
   texture creation (`kVideoWidth * scale` at window creation, texture
   width) and `s_video_pixels` buffer size to match.
2. Call `PpuBeginDrawing(g_ppu, s_video_pixels, kVideoPitch, 0)` (already
   called once at startup, `src/main.c:1072`) with the new pitch --
   already automatic once `kVideoPitch` reflects the new width.
3. Call the chosen `PpuSetExtraSpace*`/`PpuSetWidescreenLayer*` setters
   once at startup (fixed border) or per-frame (if a dynamic margin turns
   out to be wanted) -- the header explicitly notes most of these must be
   *re-applied every frame* since `ppu_reset` zeroes the fields, so a
   fixed-border approach still needs a per-frame call, just with constant
   arguments.
4. `--qualify` mode's PPM dump path (`write_ppm`, `src/main.c` around
   line 483) also hardcodes `kVideoWidth`/`kVideoHeight` -- update those
   too so `SC_DUMP_AT`/`SC_DUMP_PATH` screenshots stay usable for
   verification once this is implemented.

## Open questions (need live testing / visual inspection to resolve)

1. **Which BG layer is the map vs. the toolbar/HUD?** Strong existing
   evidence points to BG1/BG2 being the map (the View screen's tilt
   effect HDMA-drives BG1/BG2 horizontal scroll specifically, see
   `docs/INVESTIGATION_hdma.md`) and some other layer (likely BG3, given
   the `ppu.h` comments' generic "BG3 carries the HUD" framing) being the
   toolbar/status readouts -- but this hasn't been confirmed for
   the game specifically. Get this right before wiring up
   `PpuSetWidescreenLayerClamp`, or the toolbar could stretch/tile
   incorrectly into the new margins.
2. **Does the game's own simulation track tiles beyond the visible
   256px window at all?** If the game's BG tilemap only ever has valid
   data for the currently-visible + a small scroll buffer, showing more
   of it via widescreen might reveal garbage/stale tiles at the new
   edges rather than genuine extra city content. This needs visual
   verification once a first attempt renders -- `PpuSetWidescreenLayerMirror`/
   `Repeat` exist specifically as a fallback for this case (fill margins
   by reflecting/repeating the authentic edge instead of showing real
   tilemap data), so it's a known, already-solved problem in the engine
   if this turns out to be the case.
3. **How much extra width is reasonable?** `kPpuExtraLeftRight` (the
   hard clamp ceiling) wasn't read this pass -- check it before picking a
   target aspect ratio.

## Suggested next step

Pick a modest border size, wire up steps 1-3 above with
`PpuSetWidescreenLayerClamp` guessing BG3 = HUD (per the open question
above), rebuild, and take an in-game screenshot on the main map view for
a first visual check -- this is a "try it and look" task, not something
to keep scoping further without being able to see output.
