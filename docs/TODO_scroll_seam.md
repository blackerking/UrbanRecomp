# Widescreen city map: current state, open items, and dead ends

Rewritten 2026-08-27 after the work settled. The previous version had grown by
accretion -- duplicate section numbers, four "superseded" blocks, and finished
items interleaved with open ones -- and several of its sections described
approaches that were later reversed. Anything still true was carried over.

**Return point: tag `rc1`.** If a change makes the picture worse, come back to
it rather than repairing forward.

## The fault, once, properly

BG2's tilemap is 32 columns -- 256 px, exactly the screen -- and acts as a
circular buffer over a city far larger than it. Scrolling therefore has to
rewrite the column about to appear at the LEADING edge, and because 32 columns
wrap onto themselves that same column is still on screen at the TRAILING edge.
One column has to hold two different contents in the same frame. It cannot.

That single geometry produced every seam chased here:

- **trailing edge** -- the game rewrites a column still visible behind you, so
  the incoming content flashes in at the far side;
- **leading edge** -- a column becomes visible before the game rewrites it, so
  it briefly still holds the wrapped content from 256 px away.

Vertically there is no such problem: 32 rows against 224 visible lines leaves 4
spare rows to stage into, and the game rewrites a row before exposing it.
Measured over 113 frames of downward scrolling: median 0.0%, worst 12.8%,
against 24-28% spikes horizontally.

On hardware the guest's outermost columns sat in CRT overscan and were never
seen. The game is built on that assumption.

## What the code does now

**Passe-partout** (`kPassePartout = 8`, `ws_passepartout()` in `src/main.c`):
the guest's outermost tile column is never displayed. The host map -- which
draws the same terrain from WRAM -- covers 8 px on each side permanently, so
the fault is gone by construction rather than repaired per frame.
`SC_PASSEPARTOUT=0` restores the guest's own edge columns.

`ws_fix_scroll_seam()` still exists but stands down whenever the passe-partout
is on, which is the default. It is kept only as the fallback that switch
selects.

Checked before cropping anything, since the HUD lives near the edges: while the
map scrolls, columns 0-15 change 60-89% (the toolbar starts at x~16), columns
240-255 change 34-59%, and the top and bottom rows change too -- the status bar
is a panel inside the picture, not a band across the edge. An 8 px crop takes
map pixels only, at every edge.

### Measured result

| | median | peak |
|---|---|---|
| right cover, x248-255 | 0.0% | **0.0%** |
| left cover, x0-7 | 11-34% | 39-50% |

against a correctly translated previous frame, across normal, fast and diagonal
panning. Reported from play as "map scrolling itself works flawlessly - best
picture so far", so the left figure is below the visible threshold.

## Open

### 1. The left cover dips every fourth frame

The right cover matches at a flat 100.0%. The left manages 88-96% on most
frames and drops to 67-78% **every fourth frame** -- the tile-column cadence at
2 px/frame -- so it is a cell-boundary artifact in the left band, not general
misalignment. Both bands sample the same buffer the same way, which is what
makes it a puzzle.

Not visible in play. It matters anyway, because understanding it is the only
route to trusting the host cover completely.

Ruled out: that the leftmost rendered cell lacked a neighbour to receive a roof
overhang from, now that overlays extend a full cell. Rendering one cell further
left (`sx - 1`, sampling `+8`) changed the numbers by **exactly nothing**. The
extra cell was kept because it is more correct.

**Narrowed 2026-08-27: the renderer is not at fault.** Dumping `s_hostmap_px`
on consecutive frames via `SC_HOST_MAP_DUMP` shows it is cell-granular and
exact -- frames 80->81 it does not move at all, 81->82 it jumps a full 8 px,
and at its best shift it matches at **100.0%** in the left, middle and right
regions alike:

```
80->81  left +0 100.0%   right +0 100.0%   middle +0 100.0%
81->82  left +8 100.0%   right +8 100.0%   middle +8 100.0%
```

So all sub-cell motion comes from the compositor, which samples
`src[x + fx + 8]` with `fx = hScroll[1] & 7`. The suspicion is a frame where
the render's cell step and `fx`'s wrap disagree: if the origin advances a cell
on a different frame from the one where `fx` returns to 0, the sampled window
jumps 8 px and then back, at exactly the tile cadence the dip shows. The
`adj_x` reconciliation, bounded to +/-1 cell, can move that origin too.

Why that would hit the left band and not the right, when both sample the same
buffer with the same expression, is still the open part. Next: log `sx`, `fx`
and `adj_x` per frame and correlate against the dipping frames.

### 2. Roof overlay: the horizontal half is unverified

Building tops were drawn one PIXEL up-left instead of one CELL (`cell / 8`
where `cell` is 8). Fixed to `- cell`, and confirmed from play: "Roofs are fine
now". Both axes were moved, on the strength of the `-1,-1` in the format spec,
but only the vertical error was ever reported. If roofs ever look a tile too
far LEFT, that half is the suspect.

### 3. Upstream submodule: a merge, not a bump

`origin/main` is 16 commits ahead, but our submodule carries **nine local
commits** on top of merge base `9d6ad3c` -- COP modelling, `exit_mx_set`,
deadline-unwind handling, diagnostics -- touching the Rust recompiler and the
Python lowering as well as the runtime. Upstream's own commits include
`runtime: return on scheduler deadline unwind` and `Deliver every raster IRQ`,
independent solutions to problems some of ours address. Cherry-picking just the
coverage hooks onto `origin/main` conflicts immediately.

The prize is `PpuWsSetOamLeftHints` / `PpuWsSetOamRightHints`, which would let
the OBJ-clip pass -- a second full render of every line -- be deleted. Wants
its own session and its own regression pass.

Both local commits are pushed to `blackerking/snesrecomp`, branch
`simcity-host`, and `.gitmodules` points there. Before that they existed on no
remote at all, so a clean clone could not fetch the submodule.

### 4. Filed upstream, still open

- **#30** -- the new renderer drops background rectangles on halved colour math
  when extra space is non-zero. Worked around by selecting the legacy renderer
  when `PPU_halfColor && PPU_addSubscreen`.
- **#31** -- overlay capture removes OBJ from the game render but never exports
  it. No workaround; sprites cannot be borrowed, only deleted.
- **#39** -- AOT and interpreter disagree on the abs,X/abs,Y read page-cross
  penalty. Tier accuracy only; results are identical.
- #29 was filed by me and is **closed**: its headline measurement was the
  animated water, not a rendering fault. See the correction in
  `docs/upstream/ISSUE_scroll_tile_band.md`.

## Dead ends -- measured, do not retry

### Repairing the seam by translating previous-frame pixels

Worked, and is what `SC_PASSEPARTOUT=0` still selects: exact edges, median 0.0%
peak 0.0%. But it translates COMPOSED pixels, so every screen-fixed layer
inside its band moves with the map. That single property caused three separate
reports:

- the **cloned cursor** in the strip;
- the **ghosting HUD**, once the vertical block was widened to 16 rows across
  the whole guest width, dragging the status bar along;
- and it is why the repair could not simply be narrowed.

**Skipping pixels that look static** was tried as a cure and cost more than it
saved: a fast pan went from 0.0% back to **13.2%**, because genuinely wrong
pixels that happen to match the previous frame get skipped too. Screen-fixed
content cannot be separated from map content by inspecting colours after
compositing.

### Re-rendering the line with the old tilemap column

The principled fix for cloning: keep each rewritten tilemap entry's previous
value, swap it back, render the line again into a scratch surface, take the
strip from that. Sprites and HUD are drawn at their current positions, so in
principle nothing can clone.

Tried twice, worse both times:

| | translate | re-render v1 | v2 |
|---|---|---|---|
| fast right, left 16 | **1.5%** | 4.0% | 4.0% |
| diagonal, left 16 | **1.8%** | 10.9% | 8.1% |
| static pixels moved | **3.9%** | 14.5% | 10.4% |

v2 fixed the two faults v1 had -- gate evaluated once at vblank and carried, and
detection moved to the top of the frame so the first affected frame also has a
scratch -- and still lost.

**The render pass itself is exonerated.** `SC_PASS_DIAG=1` renders every line
twice and diffs: **0 of 100352 pixels differ**, every frame, even as the third
pass of the frame. An earlier version of that diagnostic compared two EXTRA
passes with each other -- neither of them the first -- and so could not have
caught what it was built to catch. So the fault was in the tilemap swap
bookkeeping, and a third attempt should dump the scratch beside the main frame
and confirm they differ ONLY in the swapped columns before assuming anything.

### The upper-tile table address

An earlier note recorded "0 drawn, 31360 skipped", cell ids `0x00..0x25`, and a
table with entries only at `0xf9..0x3ff`, and concluded the table address was
wrong. None of it reproduces. `SC_ROOF_DIAG=1` measures **1017 of 20000**
overlay tiles drawn, cell ids **0..630**, entries **120..942**. The lookup was
always working; the fault was the draw position.

## Diagnostics

| switch | what it does |
|---|---|
| `SC_PASSEPARTOUT=0` | show the guest's own edge columns; re-enables the repair |
| `SC_SEAM_FIX=0` | disable the repair (only reachable with the above) |
| `SC_SEAM_FIX_V=1` | re-enable the vertical repair (off: it dragged the HUD) |
| `SC_SEAM_LEAD_LT=1` | re-enable the left/top leading covers (off: they paint over the HUD) |
| `SC_PASS_DIAG=1` | render each line twice and diff extra pass vs MAIN render |
| `SC_ROOF_DIAG=1` | overlay-pass counts: drawn/skipped, cell id and table ranges |
| `SC_DUMP_DIR=<dir> SC_DUMP_INTERVAL=1` | record frames -- **works in interactive play now**, not just `--qualify` |

That last one matters more than it looks. It used to work only in
`run_qualification()`, so a capture session from play silently recorded
nothing -- and these defects only show while the map is moving, so they cannot
be caught in a screenshot. Every diagnosis here that needed the user's own city
depended on it.

## Also open, pre-existing

The host strip moves a different distance from the guest on ~25 of 106 frames
during fast horizontal pan. Not caused by the seam work -- disabling that
correction gives 27. Probably the "clearly apart for a split second" reported
early on.
