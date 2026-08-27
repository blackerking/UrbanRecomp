# Widescreen and the host map: what was tried, what it cost

A long attempt to give the city view a real widescreen picture, reverted on
2026-08-27. Everything here was measured, not inferred, and the numbers are the
point: several of these approaches look reasonable on paper and fail for
reasons only measurement shows. Anyone picking this up again should read the
dead ends before repeating them.

The wood-margin work described at the end SURVIVED and is not part of the
revert.

## The goal

At `SC_WIDESCREEN=96` the picture is 448 px wide. The guest draws 256. The city
view's map (BG2) is a 32-column tilemap, so it cannot simply be widened -- 32
columns is exactly 256 px, and letting it wrap tiles the city sideways. The
question was what to put in the remaining 192 px.

`SC_HOST_MAP=1` renders the map host-side from WRAM (`src/simcity_mapview.c`),
which can draw any width. The work was in making that coexist with the guest's
own picture.

## Dead end 1: the runner's overlay export

`host_map_arm_captures()` armed `PpuSetOverlayCapture` for BG3 and OBJ with
`kPpuOverlayFlag_RemoveFromGame`, intending to capture the HUD and composite it
back over the host map.

That flag does what it says: those layers are taken OUT of the game's own
render, and the HUD was deleted from the frame.

**Correction, measured later:** the export does not fail wholesale. BG3 comes
back exactly right -- the surface reports 15378 pixels on a city view where a
BG3-only render is 15378 pixels. It is OBJ alone that returns 0, against 6262
rendered. The `bg3px=0 objpx=0` line that this was originally based on is the
FIRST compose, which happens before the capture is armed; from the next frame
on it reads `bg3px=15378 objpx=0`. So the HUD's panel was recoverable all
along and only the sprites were lost. Filed upstream, see
ISSUE_overlay_obj_export.md (upstream #31).

Measured: turning `SC_HOST_MAP` on changed the GUEST's own frame. The status
bar's dark background at authentic (60,8) went from `(49,16,0)` to the map's
colour, with no other change to its code path. Not arming the capture restored
the guest frame to **100% identical** to a run with the host map off.

**Lesson:** "the map shows through the toolbar" was not a compositing failure.
The toolbar had been removed before compositing began.

## Dead end 2: capturing "everything except BG2"

The natural replacement -- render each line twice, once with only BG2 masked
off, and treat the result as the overlay.

It cannot work. The city view's HUD panel is BG3, but BG3 only becomes that
grey through colour math against BG2. Take BG2 out of the pass and the panel
does not come out grey, it does not come out at all.

Measured on the city view:

| capture | non-black pixels |
|---|---|
| everything | 53190 |
| everything except BG2 | 6262 (sprites only) |
| BG3 alone | 0 |

**Lesson:** a layer's contribution is not separable from the colour math that
composes it.

## Dead end 3: difference keying

Capture the map ALONE, and treat "guest's finished frame differs from map
alone" as the overlay. This is better -- it takes pixels from the real
composite rather than reconstructing one -- and it picks up menus opened inside
the city view for free.

It fails on black, twice over:

- A popup **windows the map off underneath itself**, so the map-alone capture
  there is backdrop. The panel's outline and lettering are backdrop-coloured
  too. Black equals black, so the test concluded "no overlay" and painted host
  map through the gaps. The disaster advice lost 1824 pixels of outline and
  text that way.
- Keeping every such pixel instead kept two black bars the width of the panel,
  above and below it, where the map is windowed off and the guest draws
  nothing.

Restricting the rescue to the span of real overlay on each row fixed the bars
without losing the lettering, but see dead end 4 -- by then the premise was
already wrong.

**Lesson:** "these two pixels are equal" is not the same as "nothing was
drawn". Ask the second question directly.

## Dead end 4: replacing the map inside the authentic picture at all

The host renderer is a reimplementation. It does not draw everything the game
draws.

Measured against the guest's own map at its best alignment:

| state | agreement |
|---|---|
| sparse map | 91.8% |
| built-up city | 76.6% |

The missing quarter is real content -- the taller roofs that overlap the tile
behind them, and shoreline decoration -- which the difference key then
discarded as "same as the map" and replaced with a flat version. Reported from
play as overlaid sprites landing in the wrong place.

**Lesson:** nothing needs to be recovered if nothing is thrown away. Keeping
the guest's 256 columns verbatim and using the host map only past its right
edge measured **0 pixels different** from the game's own frame on every screen
where the widescreen renderer is faithful. That version was the best of the
attempts and is the one to start from if this is retried.

## Measured facts worth keeping

### The host renderer never draws upper tiles

`ScMapView_Render` has a second pass for the overlapping upper halves of tall
buildings, reading a table at `SC_TILU_ADDR`. Instrumented over 20 full frames:
**0 drawn, 31360 skipped**. Every tile is rejected by its "no upper tile" test.

The table has 124 real entries, but at indices `0xf9..0x3ff`, and the map cells
read back as `0x00..0x25` -- zero overlap. A scan of 64KB of ROM found no table
with entries at the ids in use.

**This contradicts `REFERENCE_map_format.md`,** which documents exactly what
the code implements: `TILU_ADDR = TILE_ADDR - 0x77C` (file `0x014f2d`),
`v = u16 & 0x03FF`, overlay `0x300` means empty, drawn at −1,−1. The code
matches the spec, so one of the two measurements below is wrong and that is
where to start:

- the instrumented count (`0 drawn / 31360 skipped` over 20 frames), or
- the direct read of the map array, which gives **36 distinct ids on BOTH an
  empty map and a built-up one**. Two different cities cannot have identical
  tile vocabularies, so this reading is the more suspect of the two.

Resolve that contradiction before concluding anything about the table address.
Whichever way it falls, tall buildings currently have no upper halves anywhere
the host renderer draws.

### Widescreen rendering alters the guest's own picture on some screens

Same state rendered at 256 and at 448, comparing the authentic 256 columns:

| screen | pixels changed |
|---|---|
| city view | 0 |
| evaluation / history | 0 |
| advice | 8732 |
| View Mode | 1049 |

The advice is the screen whose picture is backdrop plus a HALVED subscreen. It
loses whole rectangles of map -- the black bands above and below the panel. A
256-wide render of the identical state shows city there.

**Found and fixed after this was first written.** It is the NEW renderer
(`kPpuRenderFlags_NewRenderer`), which mishandles halved colour math. On the
advice popup at full widescreen the new renderer is wrong on 8736 pixels and
the legacy one on 0. It is not a geometry effect either: 8, 32 and 96 pixels of
extra space corrupt exactly the same 8736 pixels, so enabling any widescreen at
all switches the path and the width is irrelevant.

The per-frame choice now drops to the legacy renderer whenever
`PPU_halfColor && PPU_addSubscreen` -- the "dim a scene behind an overlay"
idiom. Nothing is lost: the new renderer is chosen for its widescreen layer
policies, and such a screen has every background clamped anyway. Every screen
measured is now 0 wrong except View Mode's 1049, which has no colour math at
all (`cgadsub=$00`) and so is a different cost of the new renderer; forcing
legacy there drops it to 13 but risks the layer policies its wood margins
depend on, so it is left alone.

This is worth reporting upstream: the new renderer's halved-colour-math path
is wrong whenever extra space is non-zero.

### Colour math per screen

Read from `cgadsub` / `cgwsel`:

| screen | cgadsub | meaning |
|---|---|---|
| advice | `$60` | backdrop + subscreen, **halved** |
| evaluation, history | `$20` | backdrop + subscreen, not halved |
| graphs, comprehensive | `$a3` | main − subscreen, fixed colour 7,7,7 |
| city view | `$b3` | main − subscreen |

The halving distinguishes "dim the scene behind an overlay" from "the subscreen
IS the picture". That matters: subscreen-only layers can be let into the
margins in the first case, but in the second the panel itself is subscreen-only
and letting it through prints a second panel in each margin.

### Matching the dim cannot be done by a single factor

The popup pages darken the city with a per-pixel operation against the
subscreen. Two attempts to reproduce it on host-rendered terrain:

- **mean of edge columns** -- skewed by the panel straddling the seam, left the
  terrain 36% too dark.
- **median of per-pixel ratios** -- better, but tinted the comprehensive page
  and made the advice background uneven.

Doing it properly needs the subscreen, which the compositor does not have.

### The host map's zoom cannot coexist with extending the guest's view

`+`/`-` step the host map through 2/4/8/16/32 pixels per cell. That was fine
while the host map WAS the picture. Once it only continues the guest's view --
which is fixed at 8 -- any other cell size draws the terrain at the wrong size
AND starts it from the wrong cell, which is a band of mismatched tiles along
the join that moves as you scroll.

### One anchor, or the picture jumps

The background anchor must be identical on both sides of a screen change.
Anchoring the city view left while centring popup pages moved the terrain 96 px
the instant an advice opened and back when it closed. Where the OVERLAY is
drawn is a separate question and may differ per screen, because it is drawn
over whatever background is there.

### Vertical alignment is off by one pixel

Sweeping the offset between the guest's map capture and the host render:
`dx 0, dy +1` scores 91.8%, with nothing else within thirty points. An earlier
sweep reported "49.5%, no better alignment available" -- it had stepped `dy` in
fours and never tried +1, and that wrong conclusion is what pushed the design
toward replacing the whole map area instead of only the margins.

### Extra render passes are not free

Every additional `ppu_runLine` for the same scanline risks changing what the
guest's own render produces -- dead end 1 is a measured instance. By the end,
the city view was still running a second pass per line to feed a buffer nothing
read any more. If a pass is not needed, remove it.

## What survived

The wood-margin work is unaffected by this revert and remains in place:

- `widen_wood_bg()` lends a wood-backed layer a wood-only map for the length of
  the margin pass, then puts it back. No VRAM is written at all, which retired
  an earlier relocate-into-spare-VRAM design that collided across screens --
  screen `09` wrote its copy over screen `07`'s map and vice versa.
- `wood_grow()` walks the low four bits of a tile number to extend a row of the
  wood sheet, carrying each screen's own phase.
- `tools/wood_pattern.py` measures a tilemap's repeat and renders the match.
- Margins are blanked to the backdrop when every background is clamped, and the
  framebuffer fills stand down mid-fade.

## Current state

`SC_HOST_MAP` is off by default and its code is back to what it was before this
attempt. With it off the city view is pillarboxed. The wood screens -- main
menu, map select, name entry, both faxes, View Mode -- are widened properly,
and the flat stats pages carry their background colour out to both edges.
