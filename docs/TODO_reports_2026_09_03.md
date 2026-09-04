# Reported from play, not yet investigated

Six items reported 2026-09-03. Save states are on disk in the current format
and load on this build; they are NOT in the repo (ROM-free), so they must be
kept locally.

## 1. Scenario selector: blinking cursor at the left, pins still missing
`savestate_2.bin`

Two symptoms on one screen. The missing won-marks are already understood and
recorded at the OAM-hint block in `main.c`: the marks for the widescreen-only
columns are not in OAM at all, so no decode change can bring them back.

The **blinking cursor on the left is FIXED** (submodule `ed3c249`). It was a
second selection bracket the game blinks by flipping it 256 px left, to a
position hardware clips and the margin did not. It reached the margin through
my own `1d9cd45`: gating the edge clip on the motion grace let it through,
because every toggle looked like motion. The classifier now requires a *step*
(<= 32 px) rather than any X change. Measured: 128 stray green pixels -> 0,
with the title sign still crossing the margin correctly.

The missing pins remain open, and are a different problem entirely.

## 2. Locomotive not drawn in the widescreen margins
`savestate_3.bin` -- the state shows it as it appears in the normal view.

**Diagnosed, not fixed. It is not an OAM decode problem at all.**

The sprites are there and they are in the ambiguous band: OAM on that state
shows ten slots at raw 271-291 (the train and the traffic), while every parked
entry sits at raw 384, outside the band -- the same shape the title screen has,
where the positive decode is the right one.

But hinting them changes nothing, and neither does `SC_WS_OBJ_CLIP`: both
measured at **0 pixels difference**. The reason is further down. In the city
view the margins do not come from the PPU at all. `host_map_compose()` fills
`dst[256..447]` from the host map render and discards the PPU's own margin
columns, and the host map draws BG tiles from the city map only. No sprite can
survive that, however it decodes.

So showing the locomotive means giving the host-map compositor a sprite layer.
The machinery already exists and does not depend on the dead overlay export:
`g_snes_ppu_dbg_layer_mask` plus a scratch buffer renders a line again with an
arbitrary layer mask (`s_ws_bg_margins` uses it for the wood, `s_ws_obj_clip`
for an OBJ-less pass). An OBJ-only pass (mask `0x10`) composited over the
margin columns would do it.

### Attempt 1, reverted: the pass works, the decode does not

Built and measured, then backed out because it does not yet show anything and
should not sit in the tree enabled. What was learned is worth keeping.

The OBJ-only pass itself **works**. `g_snes_ppu_dbg_layer_mask = 0x10` plus a
scratch buffer and a `PpuBeginDrawing` retarget renders sprites in isolation --
row 110 of that save state comes back with 28 non-backdrop pixels. Compositing
it over the margin columns works too, once two mistakes are out of the way:

* Map `dst[x]` to `objlayer[x + s_ws_extra]`. `dst[kVideoWidth]` is the column
  just right of the guest's edge, which the PPU rendered at `kVideoWidth +
  s_ws_extra` in its widened frame. Only `s_ws_extra` px of the wider host
  strip can carry sprites at all; the rest has no PPU coverage.
* Test transparency on **RGB only**. The render buffer leaves the alpha byte
  clear, so comparing the whole word makes every backdrop pixel look opaque --
  which painted the entire margin solid black, 20941 px of it.

**The hint DOES reach the decode.** That earlier suspicion was wrong, and the
correction matters: there was simply nothing in the band for it to act on in
the frames being measured. The 28 pixels the pass produced on row 110 sit at
x=112..141 -- that is the vehicle at guest x 16..45, nowhere near the edge.
A "0 px difference" was measuring an empty margin, not a broken hint.

**What actually blocks it, most likely: the game culls its own sprites at the
view edge.** Evidence, none of it yet conclusive:

* Frame-differencing the whole picture over 200 frames of panning shows every
  moving object inside x < 256 and **margin = 0 on every single step**.
* With the OBJ pass composited, a 200-frame pan adds **0 px** in the margin --
  no sprite ever appears in those columns of the isolated pass either.
* The band entries (raw 271-291) render nothing even with the permissive
  decode: `SC_WS_OAM=0` against strict, with the host map off so the PPU's own
  margins are visible, is **0 px different**. So those are park slots, not
  objects, and this game parks at 271-291 as well as at 384.

If the game does cull, no decode or compositing change can ever show the
locomotive there -- it is not in OAM to be shown, and it would have to be
synthesised host-side the way the map already is.

**Do not repeat this test badly.** `SNESRECOMP_LAYER_MASK` cannot be used to
check whether something is a sprite: the host's own per-line passes
(`s_ws_bg_margins`, `s_ws_obj_clip`) write `g_snes_ppu_dbg_layer_mask` every
line and clobber it, so the env reads as having no effect. Isolate a layer with
a scratch-buffer pass instead.

The next step is to settle the culling question directly: log OAM across a pan
and see whether ANY slot ever holds a decoded x in [256, 352) while also being
drawn.

Also still to settle once it draws: the off-screen halves of clipped sprites
that `s_ws_obj_clip` exists to suppress must not come back with it.

## 3. Loan view broken in widescreen
`savestate_7.bin`

Requested: centre the screen and colour the borders like the History / TAX
pages.

**Read the centring note in the compositor first.** Moving the guest moves
everything the guest drew, HUD included, which is why advisor-page centring was
backed out twice. Colouring the borders instead of showing map removes the
map-continuity half of that problem but not the other half. Worth checking
whether the loan view has any HUD to displace -- if it does not, centring it may
be safe where the advisor pages were not.

## 4. Mouse pointer is off-spot after refocusing the window
Not a save state.

The pointer works, but its position is wrong when the cursor re-enters the
window. Likely a delta/absolute mismatch on focus regain. The host-mouse code
is the ported simcity-mouse patch in `main.c`, accumulating into `$7E01EB` (X)
and `$7E01ED` (Y).

## 5. Widescreen colours break when the mouse is used inside the menu
`savestate_8.bin`

Colour corruption tied to mouse input on a menu screen. Given the compositor
derives its subtrahend per frame from a modal host-vs-guest difference, check
whether the menu plus pointer defeats that estimator -- `SC_EXT_SUB=0` and
`SC_DIM_PROBE=1` will say quickly whether it is that or something else.

## 6. Mouse does not move at all once the menu is open
`savestate_9.bin`

Distinct from 4 and 5: no movement, not wrong movement. Suspect the menu path
stops feeding the cursor ladder, or swallows the reads.
