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

Cloning got *worse*, not better. I first assumed the extra pass could not
reproduce the main render.

**That assumption was wrong, and it has been measured.** `SC_PASS_DIAG=1`
renders every line twice into two surfaces with nothing changed between them
and diffs the result: **0 of 100352 pixels differ**, on every frame of a fast
pan. Rendering a line again is exactly reproducible, even as the third pass of
the frame. So the approach is sound and the fault was in my implementation of
it -- it is worth retrying, not abandoning.

### Retried with those fixed -- still worse. The pass is NOT the problem.

The retry evaluated the gate once at vblank and carried it, and detected the
rewrite at the top of the frame so the first affected frame also has a scratch.
It measured better than the first attempt and still lost to translating:

| | translate-previous | re-render v1 | re-render v2 |
|---|---|---|---|
| fast right, left 16 | **1.5%** | 4.0% | 4.0% |
| diagonal, left 16 | **1.8%** | 10.9% | 8.1% |
| diagonal, top 16 | **0.8%** | 10.2% | 7.1% |
| static pixels moved | **3.9%** | 14.5% | 10.4% |

`SC_PASS_DIAG=1` now compares an extra pass against the MAIN render (it used to
diff two extra passes against each other, neither of them the first, which
could not have caught a first-render difference). The result: **0 of 100352
pixels differ**, every frame. The extra pass reproduces the main render
exactly.

So the rendering mechanism is sound and both failures are in the tilemap swap
bookkeeping -- which values get restored, for how many frames, and whether the
strip is sampled from a scratch whose columns were actually swapped. That is
where a third attempt should look, and it should start by dumping the scratch
surface next to the main frame and confirming they differ ONLY in the columns
that were swapped. Do not spend more time on the pass itself.

The older, superseded guess follows.

The most likely culprit, and the thing to check first: that attempt moved the
change detection AND the `host_map_screen_live()` gate from vblank to the top
of the frame (`vPos == 0`). If the gate does not read the same at `vPos == 0`
as it does at vblank, `s_seam_active` flickers frame to frame, the strip is
patched on some frames and not others, and that alternation is exactly what the
cloning metric counts. Evaluate the gate once at vblank and carry the result
into the next frame, rather than re-deriving it at frame start.

The second thing to check is the ordering. A rewrite detected at vblank of
frame N describes a write that happened BEFORE frame N was drawn, so frame N
itself has no scratch rendered with the old columns -- only N+1 onward do. Any
retry has to decide what frame N uses.

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

## Diagnostics available

- `SC_SEAM_FIX=0` turns the repair off.
- `SC_PASS_DIAG=1` renders each line twice into two scratch surfaces and reports
  how many pixels differ. Confirms whether an extra `ppu_runLine` pass is
  reproducible before anything is built on top of one.

## Also open, pre-existing

Host strip moves a different distance from the guest on ~25 of 106 frames during
fast horizontal pan. Not caused by the seam work -- disabling that correction
gives 27. Probably the "clearly apart for a split second" reported earlier.
