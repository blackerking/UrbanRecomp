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

### The host renderer's upper tiles -- CORRECTED 2026-08-27

**Everything this section used to say was wrong.** It recorded "0 drawn, 31360
skipped" over 20 frames, map cells reading back `0x00..0x25` against a table
with entries only at `0xf9..0x3ff`, and concluded the table address had to be
wrong. Re-measured with `SC_ROOF_DIAG=1` on a built-up city:

| | claimed | actually |
|---|---|---|
| overlay tiles drawn | 0 | **1017 of 20000 (5%)** |
| map cell ids `v` | `0x00..0x25` | **0..630** |
| table entries `tu & 0x3ff` | all `0x300` | **120..942** |

The table address, the lookup and the emptiness test are all fine. The old
figures cannot be reproduced and the "36 distinct ids on both an empty and a
built-up map" reading -- flagged at the time as the suspect one -- was the wrong
measurement, not the address.

**The real fault was the draw position.** The format note at the top of
`simcity_mapview.c` and `REFERENCE_map_format.md` both say the overlay is drawn
at -1,-1. The code shifted by `cell / 8`, which at the native cell size of 8 is
a single PIXEL, so every tall building's upper half sat 7 px too low. Reported
from play as the roof tiles being about a tile below where they belong -- which
is what identified it. Now `- cell`.

Note this scaled the wrong way too: at `SC_MAP_ZOOM` cell sizes of 16 or 32 the
old expression gave 2 px and 4 px, so the error grew with the zoom.

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

## The motion correction latched, and only the disaster camera showed it

Reported from play: after a disaster, the extension sits **one tile off in both
axes**, and *only* when the disaster camera moves the map -- ordinary panning
with A is fine. "Only after the cam" is the signature of a latch rather than a
tracking error, and that is what it was.

`adj_x`/`adj_y` correct a one-frame lag between the game's cell scroll
(`$01bd`/`$01bf`) and the PPU's BG2 register, carried as whole cells and
clamped to +/-1. The comment asserted "at rest it is zero and the picture is
untouched". That was not true. Nothing drove the value back to zero: it was
cleared only by a **jump** of 32 px or more, so any standing discrepancy the
correction happened to pick up stayed forever.

Measured on `savestate_6.bin` with `SC_COMPOSE_DIAG=1`, the disaster camera pans
the view and the adjustment latches:

    [compose] f=1    sx=21 sy=61 dsx=0 fx=0 fy=0 adj=0,0
    [compose] f=13   sx=21 sy=71 dsx=0 fx=0 fy=0 adj=0,-1
    [compose] f=121  sx=21 sy=71 dsx=0 fx=0 fy=0 adj=0,-1
    ...
    [compose] f=349  sx=21 sy=71 dsx=0 fx=0 fy=0 adj=0,-1

350+ frames with `dv`, `dh`, `fx` and `fy` all zero -- nothing moving anywhere
-- and the correction still subtracting a cell.

The fix restores the documented invariant: after **eight consecutive frames**
with neither the register nor the cell moving, the adjustment decays to zero.
Eight because the guest scrolls 2 px at a time and a pan never goes that long
without moving, so this cannot fire mid-pan and undo the lag correction it
exists to provide. Verified both ways:

* Panning (A + Right) still engages it -- `adj=-1,0` holds across the whole pan.
* At rest it is `0,0`, on every one of the save states that reach the map screen.

`SC_HOSTMAP_ADJ=0` disables the correction outright, for isolating it.
`SC_COMPOSE_DIAG=1` now also prints `sy`, `fy` and the live `adj` pair, which is
what made the latch visible at all -- it previously printed only the horizontal
half, and this fault is mostly vertical.

**Not caused by the load accelerations.** Checked before touching anything:
composing the same frame with `SC_MAPCLS=0 SC_DECOMP_FAST=0` and with both on
gives the same picture apart from one animation step on a sprite.

A scoring metric was tried first -- comparing the host strip against the guest's
own centre columns, the method that established the "one row up" constant -- and
**thrown away rather than trusted**: it reported 1.3% agreement for every
variant, because the state under test has a dialog covering the centre and
because the host buffer's ARGB does not compare bit-exactly against the PPU's
output. A metric that cannot tell right from wrong is worse than none.

## Subtractive colour math: the map screens

Reported from play: on the Maps Screen the extension is not darkened the way it
is on the advisor pages.

The advisor pages are `cgadsub $60` -- additive, halved -- which the `halve`
branch already reproduced. The map screens are **`cgadsub $a3`**: subtract, NOT
halved, operand = subscreen. `halve` never fired, so the guest darkened its own
city and the extension stayed at full brightness.

The runner does not expose the subscreen, so the math cannot be applied
directly. It does not need to be. The host strip spans the guest's **own**
columns, so at the same screen x both draw the same cell and the difference is
exactly what the math did:

    host  b5 94 73   ->   guest  7b 5a 39
          181 148 115         123  90  57      difference: 58, 58, 58

A uniform subtrahend, derived rather than guessed. So each frame the compositor
takes the **mode** of (host - guest) per channel over the overlap and subtracts
it from the extension.

Three things this gets wrong if done naively, all of them found by measurement:

1. **The mean and the median are useless here.** The overlap also holds HUD, the
   panel and sprites, where the two legitimately differ. This is the same rock
   two earlier attempts to fit a ratio hit (36% too dark; a tint on other
   pages). The mode is the value the majority of city pixels agree on.

2. **Clamped channels lie.** Where the subtraction drove a channel to zero the
   observed difference is smaller than the real subtrahend: `savestate_0` gives
   host `00,31,ad` -> guest `00,00,73`, i.e. diffs `0,49,58`, where the truth is
   58 everywhere and green merely ran out of range. Counting those biases each
   channel by a different amount -- a colour cast, not a dimming. Only unclamped
   channels are counted.

3. **The `cgadsub` subtract bit is not sufficient.** `savestate_7` is `$b3`,
   which has that bit set, and subtracts nothing at all -- guest and host are
   pixel-identical there. Estimating anyway returned **107**, which would have
   darkened ordinary gameplay badly. So when a large share of the overlap is
   identical, the answer is zero regardless of what the register says. Only the
   frame can settle it.

Derived subtrahend, by state:

| state | cgadsub | result |
|---|---|---|
| `savestate_4` (LAND VALUE) | `a3` | 58,58,58 |
| `savestate_0` (map screen) | `a3` | 58,58,58 |
| `savestate_7`, `_3`, `_5` (play) | `b3` | 0,0,0 |
| `savestate_6` (advisor) | `60` | 0,0,0 -- `halve` handles it |

Effect on `savestate_4`: the right extension goes from mean brightness 117.1 to
68.8 against the guest area's 76.8, which it previously overshot by 50%. The
guest's own columns are untouched (77.2 -> 76.8; the residue is the left margin
inside the sample window).

`SC_EXT_SUB=0` disables it. `SC_DIM_PROBE=1` prints the derived subtrahend and
sample guest/host pixel pairs, which is how all of the above was measured.

## Fades: brightness was handled, force blank was not

Reported from play: leaving the Information menu for View Mode, "the widescreen
is fading into black, getting back at full brightness, before it becomes
overwritten with wood".

The host map already dims with a fade -- `pal_entry()` runs its colours through
the PPU's `brightnessMult` table for exactly that reason. That covers one of the
two ways a SNES shows nothing, and missed the other: **INIDISP bit 7**.

Measured across the transition, per frame, mean brightness of the guest's own
columns against the extension:

| frame | guest | extension |
|---|---|---|
| 64-77 | 95.5 -> 6.5 | 89.4 -> 6.3 |
| **78** | **0.0** | **100.6** |
| 79-91 | 0.0 | 100.6 |
| 92-95 | 0.0 | 2.2 |

The fade itself tracks correctly. What breaks is the end of it. `SC_COMPOSE_DIAG`
shows why: `inidisp` steps `09, 08 ... 01`, and then the game writes **`8f`** --
force blank ON, brightness restored to **15** -- so it can rebuild the screen
unseen. `brightnessMult[15]` is exactly what it was before the fade began, so
the extension springs back to full brightness for fourteen frames while the
guest displays black.

Brightness alone can never express this: the register reads 15 and means
*nothing is displayed*. So the compositor now checks `PPU_forcedBlank()` and
paints the extension black, on both the plain and the halved paths.

After the fix the extension reaches 0.0 on the same frame the guest does and
stays there; the largest guest/extension gap anywhere in the fade is 5.0, which
is the two regions holding different terrain, not a brightness mismatch. Frames
outside a blank are pixel-identical to before.

`SC_COMPOSE_DIAG` now also prints `inidisp`, the force-blank flag and the
brightness, which is what made this visible in one run.
