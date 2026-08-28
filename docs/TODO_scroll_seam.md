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

## 5. DONE -- overlapping building parts

Fixed 2026-08-27. It was NOT the table address, and the note that said the
upper-tile pass draws nothing was wrong -- `SC_ROOF_DIAG=1` measures 1017 of
20000 overlay tiles drawn, cell ids 0..630, table entries 120..942. The lookup
was always working.

The fault was the draw position: `cell / 8` instead of `cell`, i.e. one PIXEL
up-left instead of one CELL, leaving every tall building's upper half 7 px too
low. Identified from play as the roofs sitting a tile below where they belong.
See `docs/WIDESCREEN_HOST_MAP.md`.

## 5b. Superseded note on building parts

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

## 3. DONE -- the LEADING edge (right seam)

Reported as: moving right WITHOUT S, a stripe of foreign content at the guest's
right edge, which widescreen puts in the middle of the picture next to the host
map join. Fast movement was fine.

It is the mirror of the trailing-edge fault. A tile column becomes visible at
the leading edge BEFORE the game rewrites it, so for two or three frames it
still holds the wrapped content from 256 px away, then snaps correct. At
2 px/frame the column is exposed for ~3 frames before the write; at 4 px/frame
barely one, which is why only the slow pan showed it.

Captured from play, right 16 px spiked every FOURTH frame -- one tile column --
to 72-79% "correctly scrolled" against 84-86% on quiet frames, and it was
neither stale (43%) nor wrapped-by-256 (27%): a one-off content flip.

It cannot be repaired from history like the trailing edge, because the correct
pixels do not exist yet anywhere -- the game has not written them. The host map
has that terrain from WRAM, so the strip now starts a few pixels early to cover
the sliver, and only while it is actually wrong (`s_seam_lead_dirty`). Zero
cover on every frame the guest's own edge is correct.

Note this only works on the RIGHT. The guest is left-aligned in the composed
frame, so there is no host map to the left of it; scrolling left, the leading
edge is the frame's left edge and has no cover available.

**On hardware this sliver sat in CRT overscan and was never visible.** Widescreen
is what exposed it.

## 3b. Left and top leading edges -- BUILT BUT UNVERIFIED

The same cover extended to the other two leading edges (scrolling left, and up).
**Neither has been reproduced, so neither is measured.** Plain Left and plain Up
do not pan the map from any save state here -- the city is already at its
boundary in those directions -- and the play capture contains only rightward and
downward scrolling. The code is symmetric with the right edge, which is the only
argument for it so far.

Two things to check when a repro exists:

- **These edges are where the HUD lives.** The right edge sits in open picture
  next to the join; the left is the toolbar and the top is the status bar, so
  the cover paints host terrain over them for the two or three frames it is
  active. `SC_SEAM_LEAD_LT=0` disables both without touching the right edge.
- Whether the artifact is even there. At 4 px/frame the leading column is
  exposed for about one frame before the game rewrites it, which is why holding
  A hides the right-hand seam; the same may make left and up a non-issue.

The BOTTOM leading edge was measured in the capture and needs no cover: 113
frames of downward scrolling give a median of 0.0% and a worst case of 12.8%,
against 24-28% spikes on the right edge. Vertically the tilemap has 4 spare
rows to stage into, so the row is normally written before it is exposed.

## 6. Passe-partout -- adopted on the RIGHT edge only

The idea: never display the guest's outermost tile column, since every seam in
this file lives in exactly those 8 px and on hardware they sat in CRT overscan.
The host map covers them instead, permanently, so the fault is gone by
construction rather than repaired frame by frame.

Checked first that this is safe: while the map scrolls, columns 0-15 change
60-89%% (the toolbar starts at x~16), columns 240-255 change 34-59%%, and the
top and bottom rows change too -- the status bar is a panel inside the picture,
not a band across the edge. An 8 px crop takes map pixels only.

**It only wins on the right.** Measured on savestate_5 against a correctly
translated previous frame, median (peak):

| | repair | passe-partout |
|---|---|---|
| left x0-7 | **0.0% (0.0%)** | 11-34% (39-50%) |
| right x248-255 | 0.0% (**39-57%**) | 0.0% (**0-3.7%**) |

So the two are used together: passe-partout owns the right edge, the repair
owns the left. Combined, both edges are median 0.0% with a peak of 0.0% except
one 3.7% frame. The right edge used to peak at 39-57%.

Why the host cover loses on the left is NOT understood. The cover tracks the
guest correctly there (same shift on 111 of 129 frames) yet still mismatches
11-34%%. One hypothesis was tested and disproved: that the leftmost rendered
cell lacked a neighbour to receive a roof overhang from, now that overlays
extend a full cell. Rendering one cell further left and sampling 8 px in
changed the numbers by nothing at all. That neighbour cell was kept anyway --
it is more correct -- but the cause is still open.

Because the repair still runs for the left edge, **the cloned cursor and HUD
are still possible there**. Extending the passe-partout to the left would
retire the repair entirely, and with it the cloning; that is the prize if the
left-edge question above is ever answered.

At rest the change is confined to columns 248-255, verified on six save states.
`SC_PASSEPARTOUT=0` restores the guest's own right-edge column.

## 7. HUD ghosting -- the repair was dragging the HUD along

Reported from play once the map itself was clean: HUD elements drawn a frame
late, ghosting at the top left and at the far right when panning up. The map
was explicitly fine.

Two things I had added were responsible, and both were low value:

- **The vertical repair.** The horizontal one touches 16 columns of map; the
  vertical one rewrote 16 rows across the WHOLE guest width, straight through
  the status bar, translating whatever HUD sat in them. It also bought nothing:
  the tilemap has 4 spare rows vertically, so the game rewrites a row before it
  is exposed -- 113 frames of downward scrolling measured median 0.0%, worst
  12.8%, against 24-28% spikes on the horizontal axis where there is no slack.
  Now off by default; `SC_SEAM_FIX_V=1` restores it.
- **The left and top leading covers**, shipped unverified with a warning that
  they paint over the toolbar and status bar. They do. Off by default;
  `SC_SEAM_LEAD_LT=1` restores them.

After: left edge median 0.0% (peak 0.0%), right 0.0% (peak 3.7%) across normal,
fast and diagonal panning -- the map quality is unchanged.

**The lesson, again:** anything that translates COMPOSED pixels moves the
screen-fixed layers with the map. That is the same root cause as the cloned
cursor. The horizontal repair still has it, over 16 columns of map where the
HUD rarely is, which is why it is tolerable there and was not on the full-width
vertical band.

## 8. Passe-partout on BOTH edges; the repair is retired

Reported from play after the vertical fixes: "left is the big problem, right
the small one" -- HUD elements ghosting a frame behind. That is the horizontal
repair, the last thing still translating COMPOSED pixels, so screen-fixed
layers inside its 16 columns are dragged along with the map. Narrowing it again
would not have cured it; only removing it does.

So the passe-partout now covers both edges and `ws_fix_scroll_seam()` stands
down entirely. No repair, no ghosting, no cloned cursor -- those all came from
the same mechanism.

**The cost, measured.** The right cover is exact: 100.0% match against a
correctly translated previous frame, every frame. The left cover is not:

```
frame   left x0-7 match at +2      right x248-255
  79          93.6                     100.0
  82          78.1   <- dip            100.0
  86          75.4   <- dip            100.0
  90          67.0   <- dip            100.0
```

88-96% on most frames, dipping to 67-78% **every fourth frame** -- the tile
column cadence at 2 px/frame. So it is a cell-boundary artifact in the left
band specifically, not general misalignment, and the right band sampling the
same buffer the same way is perfect.

That asymmetry is still unexplained. Tested and disproved: that the leftmost
rendered cell lacked a neighbour to take a roof overhang from. Rendering one
cell further left and sampling 8 px in changed the numbers by exactly nothing.
The extra cell was kept because it is more correct.

`SC_PASSEPARTOUT=0` goes back to the repair -- exact edges, ghosting HUD.

## Build debt found while firming up rc1

`SimCityAOTProbe` and `SimCityAOTDiff` do not link:

```
simcity_fiberdrive.obj : error LNK2019: unresolved external symbol
    sc_advance_until_input_ready referenced in SimCityFiberDrive_RunGuestFrame
```

`sc_advance_until_input_ready()` is defined in `src/main.c` (line ~1580), which
those targets do not compile -- they use `src/aot_probe.c` as their entry point
while still pulling in `src/simcity_fiberdrive.c`, which calls it.

**FIXED** by the second route: nothing in either target uses the
`SimCityFiberDrive_*` API -- only `src/main.c` does, and that is not compiled
into them -- so `src/simcity_fiberdrive.c` was simply dead weight there and is
no longer linked. All five targets now build and both tools run.

Stubbing the symbol would have been the wrong fix, and the CMakeLists comment
says so: it drains `autoJoyTimer` before the guest is handed a frame, and
skipping that is exactly how the first fiber attempt deadlocked -- the game
spins on `$4212` at 00:9280 and nothing advances the beam while the guest holds
the CPU. A stub links and then hangs.

**First run of `SimCityAOTDiff` since it was repaired: cycle counts agree on
51 of 64 trials.** First look below; the cause is NOT found.

### What the 13 disagreements look like

Results are fine -- `divergent: 0`, so WRAM and A/X/Y come out identical. Only
the cycle accounting differs, and the AOT tier is always HIGH, always by a
multiple of 8:

```
00:8982  AOT 760 vs interp 744  (+16)   on 4 of 8 trials
00:8924  AOT 760 vs interp 744  (+16)   on 1 of 8
00:D23A  AOT 240 vs interp 232  (+8)    on 1 of 8
```

**It is trial-dependent**, which is the strongest clue: the same body agrees on
most randomised trials and disagrees on the rest. So the extra charge is
conditional on entry state, not on the instruction stream.

`00:D23A` is the small case and the one to work on:

```
REP #$30 / LDA $0421 / ASL A / TAX / LDA $d193,X / STA $7e2000 / RTS
```

### Ruled out

- **A per-`STA long` overcharge.** It correlates perfectly -- D23A has one long
  store and is +8, the two +16 bodies have two each -- but the generated block
  for D23A charges `master_cycles += 232`, which is exactly what the
  interpreter reports. The static cost is right, so the extra 8 is added
  somewhere else, and the correlation is a coincidence of those three bodies.
- **The AOT prologue's stack reads.** The generated function recovers the host
  return PC with `cpu_read8()` when `host_return_valid` is 2 or 3, which varies
  per trial and looked like an excellent fit. But `cpu_read8()` returns early
  for WRAM via `cpu_wram_offset()` and charges nothing; only hardware registers
  reach `cpu_pace_cycles()`. The stack is WRAM.

### Where to look next

The conditional `+= 8` charges that DO exist in generated code -- e.g.
`if (cpu->D & 0xFF) { cpu->cycles += 1; cpu->master_cycles += 8; }` for the
direct-page penalty, and branch-taken penalties. Both are entry-state
dependent, which matches the trial-dependence. D23A uses no direct-page
addressing and does not branch, so if one of those is firing in its block, that
is the bug. Dump `cpu->D` and the taken/not-taken path per trial and correlate
against the disagreeing trials.

Note this is tier accuracy, not correctness, and nothing in the rendering work
depends on it.

## Upstream submodule: a merge, not a bump

`origin/main` is 16 commits ahead, but our submodule carries **nine local
commits** on top of the merge base `9d6ad3c` -- not just the coverage hooks:

```
bedf078 interp_bridge: optional host coverage hooks
edc35da interp_bridge: bounce and interpreted-step counters
8b08f0d interp_bridge: SNESRECOMP_REGWRITE_DIAG
a6a037f interp_bridge: hand a deadline unwind back to the host
2c06601 interp_bridge: report the deadline when it FIRES
2a095a1 interp_bridge: pctrace markers, APU bisect switch
93d4dd1 interp_bridge: two diagnostics for runs that never return
3dbd292 cfg: add exit_mx_set for callees that exit in several widths
0183d9a Model COP as a tier-to-LLE call instead of structural poison
```

They touch the Rust recompiler (`cfg.rs`), the Python lowering and the runtime.
Upstream's own 16 include `runtime: return on scheduler deadline unwind` and
`Deliver every raster IRQ, and run unresolved dispatch indices` -- independent
solutions to the same problems `a6a037f` and friends address. A cherry-pick of
just the hooks onto `origin/main` conflicts immediately.

So this is a real merge with functional overlap in the runtime, and it wants
its own regression pass (qualify plus the ten-state comparison) rather than
being folded in beside rendering work. The prize is
`PpuWsSetOamLeftHints`/`PpuWsSetOamRightHints`, which would let the OBJ-clip
pass -- a second full render of every line -- be deleted.

## Diagnostics available

- `SC_SEAM_FIX=0` turns the repair off.
- `SC_DUMP_DIR=<dir> SC_DUMP_INTERVAL=1` now records frames from the INTERACTIVE
  loop too. It used to work only under `--qualify`, so a capture session from
  play silently recorded nothing -- which matters because these defects only
  show while the map is moving and cannot be caught in a screenshot.
- `SC_PASS_DIAG=1` renders each line twice into two scratch surfaces and reports
  how many pixels differ. Confirms whether an extra `ppu_runLine` pass is
  reproducible before anything is built on top of one.

## Also open, pre-existing

Host strip moves a different distance from the guest on ~25 of 106 frames during
fast horizontal pan. Not caused by the seam work -- disabling that correction
gives 27. Probably the "clearly apart for a split second" reported earlier.
