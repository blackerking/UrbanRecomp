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

Both covers are **exact**: 100.0% match against a correctly translated previous
frame, every frame, on both edges, across normal, fast and diagonal panning.
Reported from play as "map scrolling itself works flawlessly - best picture so
far".

## Open

### 1. RESOLVED -- the "left cover dip" was a measurement artifact

This section previously recorded the left cover as mismatching 11-34% with dips
to 67-78% every fourth frame, against a flat 100.0% on the right, and called
that an unexplained asymmetry. **It was my metric, not the picture.**

The band test compared an 8 px cover against a window shifted by the scroll
delta. Shifted by +2, `x0-7` reads `x2-9` -- and `x8, x9` are GUEST pixels, so
two of the eight columns were comparing host content against guest content
every frame. The right band never had the problem: shifted, `x248-255` reads
`x250-257`, and `x256+` is more host strip, so it stayed host-vs-host. That
asymmetry was the whole "puzzle".

Measured with a window that stays inside the cover (`x0-5` shifted by +2), the
left band is **100.0% on every frame**, exactly like the right.

The dips clustering on `fx=6` frames was the same artifact seen through the
fine-offset cycle, not a cell-boundary fault.

Two things were ruled out along the way and are worth keeping, because both
were plausible and both are now excluded:

- **The renderer.** `SC_HOST_MAP_DUMP` on consecutive frames shows it is
  cell-granular and exact -- frames 80->81 it does not move, 81->82 it jumps a
  full 8 px, matching at 100.0% in every region.
- **The compositor's sampling.** `SC_COMPOSE_DIAG=1` prints `sx`, `fx` and the
  combined origin per frame: it advances by exactly +2 every frame, with the
  cell step (`dsx=1`) landing precisely when `fx` returns to 0. No jump.

**Lesson for the next measurement:** a band metric that shifts its window must
keep that window inside the region it is judging. Otherwise it reports the
boundary between two correct things as a defect.

### 2. Roof overlay: the horizontal half is unverified

Building tops were drawn one PIXEL up-left instead of one CELL (`cell / 8`
where `cell` is 8). Fixed to `- cell`, and confirmed from play: "Roofs are fine
now". Both axes were moved, on the strength of the `-1,-1` in the format spec,
but only the vertical error was ever reported. If roofs ever look a tile too
far LEFT, that half is the suspect.

### 3. DONE -- upstream merged; one inherited clone defect

The nine local commits and upstream's sixteen merged with a single conflicted
file, `runner/src/snes/interp_bridge.c`, in four hunks -- all the
deadline-unwind machinery, where `s_lle_unwind_from_deadline` (ours) and
`s_lle_unwind_is_deadline` / `s_lle_next_unwind_is_deadline` (upstream) are two
answers to one question. Upstream's won: same behaviour on expiry, plus it
clears `s_lle_unwind_active` and the owner depth, which ours did not. Kept only
what is additive on our side -- the coverage hooks `src/main.c` wires, and
`SNESRECOMP_DEADLINE_DIAG`.

**Verified rendering-neutral**: 30 frames of scrolling gameplay from
`savestate_5` are byte-identical before and after, 0 pixels differing. All five
targets build; qualify PASS at 2000 and 6000.

Two build fixes were needed, both recorded in the merge commit: a stub for
`wlog_addr_note_direct()` (upstream's `snes.c` now logs direct WRAM writes
through a function in `cpu_state.c`, which this target deliberately excludes --
safe to stub because it is purely a diagnostic), and MSVC portability for
`__attribute__((constructor))`, which MSVC rejects outright.

Submodule now at `blackerking/snesrecomp` branch `simcity-host-main`.

#### Inherited: a nested submodule with no URL

`snesrecomp/.gitmodules` declares `lib/retcomm-rbengine` with a `path` and a
`branch` but **no `url`**, so `git clone --recurse-submodules` stops with
"No url found for submodule path". Identical on `origin/main`, so it is
upstream's defect, not the merge's -- but the pre-merge pointer did not carry
that gitlink, so this is a regression in clone-ability that arrived with it.

The outer submodule still checks out and the build is unaffected; only the
recursive clone errors. Filed as
[#41](https://github.com/mstan/snesrecomp/issues/41). If it is fixed by
supplying the URL, do not guess it -- `lib/recomp-net` points at
TechnicallyComputers, but that is an inference, not knowledge.

### 4. Filed upstream, still open

- **#30** -- the new renderer drops background rectangles on halved colour math
  when extra space is non-zero. Worked around by selecting the legacy renderer
  when `PPU_halfColor && PPU_addSubscreen`.
- **#31** -- overlay capture removes OBJ from the game render but never exports
  it. No workaround; sprites cannot be borrowed, only deleted.
- **#39** -- AOT and interpreter disagree on the abs,X/abs,Y read page-cross
  penalty. Tier accuracy only; results are identical.
- **#40** -- `__attribute__((constructor))` in `interp_bridge.c` does not
  compile on MSVC. Fixed on our fork; the fix is offered in the issue.
- **#41** -- `lib/retcomm-rbengine` has no `url` in `.gitmodules`, so recursive
  clone fails.
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

## OPEN -- title: the Maxis / SimCity building parts move wrongly

Reported from play 2026-08-27, straight after the light-row fix below. Not yet
investigated at all; this entry is the report and a starting point, nothing
more.

The title animates its logo out of building pieces. Those pieces are moving
wrongly. Which way is wrong -- speed, direction, offset, or only in the
widescreen margins -- is NOT recorded, so establish that first from a capture
rather than guessing.

Where to start:

1. `SC_DUMP_DIR=<dir> SC_DUMP_INTERVAL=1` while the title plays, then measure
   the pieces frame to frame. Interactive capture works now, so a real session
   can be recorded and analysed.
2. `SC_LAYER_MASK` to find out WHAT draws them -- bit0 BG1, bit1 BG2, bit2 BG3,
   bit3 BG4, bit4 OBJ. The light row is OBJ; if the building pieces are OBJ too,
   `widen_title_lights()` is the only host code touching title sprites and is
   the first suspect. If they are a background, the margin/clamp path is.
3. Compare against `SC_WIDESCREEN=0`. If the motion is correct at authentic
   width, it is ours; if it is wrong there too, it is the guest or the device
   model, and none of the widescreen code is implicated.

Note the light-row bug found the same day was a host bug in title sprite
handling, so step 2 is worth doing before anything else. But do not assume it:
the two may be unrelated.

## Title lights: fixed 2026-08-27

Reported from play: on the title, the widescreen light row was missing on the
LEFT for the first frames, appearing only once the title started to move.

Measured with `SC_LAYER_MASK=0x10` (sprites only): **0 px in the left margin
against 1293 in the right**, on every title frame. It never worked; what shows
once the title moves is the game's own sprites entering the margin.

`widen_title_lights()` measured the row's extent over every OAM member sharing
the row's Y/tile/attribute -- including one parked off-screen at **x = -255**.
That made `lo = -255`, so the leftward loop started at `lo - pitch = -319` and
its first condition (`x >= -extra - pitch`) was already false. It placed
nothing, ever: `placed L=0 R=2`.

The extent is now measured only over members the authentic viewport shows
(`-16 < x < 256`), which gives `lo = 1` and `placed L=2 R=2`. Left margin
renders 1296 px against the right's 1293.

The left OAM hints (`PpuWsSetOamLeftHints`, from the upstream merge) were wired
up at the same time, mirroring the right. They turned out NOT to be the cause --
strict versus permissive decode is 0 differing pixels on four save states -- but
they are kept: our own placed slots are now claimed explicitly on both sides
rather than relying on the permissive default.

`SC_LIGHTS_DIAG=1` prints the row the extender found: member count, y, lo, hi,
pitch, and how many sprites it placed each side.

## Also open, pre-existing

The host strip moves a different distance from the guest on ~25 of 106 frames
during fast horizontal pan. Not caused by the seam work -- disabling that
correction gives 27. Probably the "clearly apart for a split second" reported
early on.
