# Scroll seam: what is left

Picked up from play on 2026-08-27, after the seam repair landed in 1185664.

**RETURN POINT: commit `1185664` (tag `seam-good`).** Reported from play as
"much lesser visible than before". If a later attempt makes the seam worse,
come back here rather than trying to repair forward.

Current mechanism, for anyone starting cold: BG2's tilemap is 32 columns --
256 px, exactly the screen -- and is a circular buffer over a much larger city,
so the column entering at the leading edge is still on screen at the trailing
edge. `ws_fix_scroll_seam()` in `src/main.c` patches the trailing 16 px from the
previous frame, translated by the scroll delta, held until the rewritten column
scrolls off. Background is in `docs/upstream/ISSUE_scroll_tile_band.md`.

## 1. Cloned cursor inside the seam

Cursor at the extreme left or right while the screen moves the OTHER way
produces a second copy of the cursor in the repaired strip.

**Cause is known.** The repair copies the previous frame's *composed* pixels,
which include sprites. A sprite sitting in the 16 px strip gets reprinted at its
old position. I measured the residual with sprites masked off early on and saw
no difference, but that test never had the cursor at the edge -- it does not
clear this.

Fix direction: the patch needs BG2's contribution only, not the composite.
Options: render the strip separately with a BG2-only pass, or keep a
sprite-coverage mask for the strip and skip those pixels.

## 2. Diagonal movement breaks it (SS2)

Cursor in the lower-left corner, moving left and down together: the RIGHT seam
is heavy and clearly visible.

The horizontal and vertical patches are independent blocks and neither knows the
other ran. Suspects, in order: the trailing edge for leftward motion is the
right side and has thinner test coverage than rightward (plain Left never panned
the map from savestate_2, so those runs measured nothing); and the corner where
both strips meet is patched twice, the second read coming from already-patched
pixels.

Start by building a repro that actually pans diagonally -- input mask
`kPad_Left|kPad_Down|kPad_A` = `0x0160` -- and confirm the map moves on BOTH
axes before measuring anything.

## 3. Patched strip drifts (SS3)

Cursor in the upper-right corner: the left seam is "clearly moving, only to the
left". Sounds like the repaired sliver translating when it should be still, i.e.
the hold width decrementing out of step with the actual scroll delta, or the
chain re-reading its own output. `s_seam_hold_x` decrements by `|dx|` per frame;
check it against what the register really moved that frame.

## 4. Bottom seam when panning down (SS5, low priority)

**Cheap and known.** The vertical patch was never widened. Horizontal was raised
from 8 to 16 px once measurement showed the game rewrites TWO columns per update
("wrote: 6 7"); the vertical block still sets `s_seam_hold_y = 8` and still
tests only the trailing row, where the horizontal one tests both edge columns.
Make it symmetric with the horizontal block first and re-measure -- that may be
the whole fix.

Note 230 frames of vertical panning from savestate_2 found NO trailing-edge
spike, so a repro for this needs a different spot in the city.

## 5. Overlapping building parts not visible

Tall buildings whose upper half overlaps the tile behind them are missing.

Almost certainly the host map strip, not the guest: `ScMapView_Render`'s second
pass for upper tiles draws nothing. Instrumented over 20 frames it was **0 drawn,
31360 skipped** -- every tile rejected by its "no upper tile" test. The table at
`SC_TILU_ADDR` has 124 entries at indices `0xf9..0x3ff` while the map cells read
back `0x00..0x25`, so they never intersect.

Before chasing the table address, resolve the contradiction recorded in
`docs/WIDESCREEN_HOST_MAP.md`: the same read gives **36 distinct ids on both an
empty map and a built-up one**, which cannot be true of two different cities.
That reading is the more suspect of the two measurements.

## Also open, pre-existing

Host strip moves a different distance from the guest on ~25 of 106 frames during
fast horizontal pan. Not caused by the seam work -- disabling that correction
gives 27. Probably the "clearly apart for a split second" reported earlier.
