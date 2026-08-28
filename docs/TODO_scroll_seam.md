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

## 1. Cloned cursor and HUD inside the seam

**STILL OPEN. One approach has been tried and measured; do not repeat it.**

The repair copies the previous frame's *composed* pixels and translates them by
the scroll delta, so anything that does NOT scroll with the map -- the cursor,
the HUD -- gets reprinted at its old position.

### Tried and rejected: skip pixels that look static

Skip a pixel when it is unchanged at the same screen position between frames.
It cost far more than it saved: a fast pan went from 0.0% back to **13.2%**,
because genuinely wrong pixels that happen to match the previous frame get
skipped too. Screen-fixed content cannot be separated from map content by
inspecting colours after compositing.

### Tried and rejected: re-render the line with the old tilemap column

The principled version. Keep each rewritten tilemap entry's previous value,
swap it back into VRAM, render the line a second time into a scratch surface
(`PpuBeginDrawing` + `ppu_runLine`, the same shape the OBJ-clip pass uses), and
take the trailing strip from that. Sprites and HUD are drawn at their CURRENT
positions in that pass, so in principle nothing can clone.

Measured, it was worse on every axis:

| | translate-previous | re-render |
|---|---|---|
| fast right, left 16 | 0.0% | 4.0% |
| diagonal, left 16 | 0.0% | 10.9% |
| diagonal, top 16 | 0.8% | 10.2% |
| static pixels moved (cloning) | 3.9% | **14.5%** |

Cloning got *worse*, which is the tell: the scratch pass does not reproduce the
main render. The city view already renders each line twice (the OBJ-clip pass),
and a third pass appears not to see the same sprite-evaluation state, so the
scratch differs from the real picture in exactly the places that were supposed
to be preserved. Reverted.

**Before trying again, settle that first**: render a line twice into two
surfaces with NO tilemap swap and diff them. If they are not identical, the
extra-pass approach cannot work as written, and that is the thing to fix --
`docs/WIDESCREEN_HOST_MAP.md` already records that extra `ppu_runLine` passes
are not free.

## 1b. Original note on the clone

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

## 2. DONE -- diagonal, pauses, bottom edge

Fixed 2026-08-27, after `seam-good`:

- **Diagonal.** Each block translated on its own axis only -- the horizontal
  patch read `prev[y][x+dx]` and ignored `dy` -- so a diagonal pan pulled pixels
  from the wrong row and the repair itself painted a seam. Both axes now.
- **Paused frames.** `else { hold = 0; }` fired whenever the map did not move,
  and cursor panning stops constantly (16 moving frames against 113 paused in
  the repro), so one paused frame abandoned the repair. The hold now survives,
  capped at 12 idle frames, and only arms near movement.
- **Bottom edge.** The vertical block was still 8 px and one edge row while the
  horizontal had gone to 16 and both columns. Now symmetric.

Measured: fast right 67.6% -> 1.5%, diagonal 55.7% -> 1.8%, top edge 7.1% ->
0.8%. Idle inert on all ten states, qualify PASS.

## 2b. Superseded notes on diagonal movement (SS2)

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
