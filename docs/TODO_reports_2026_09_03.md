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

**The hint DOES reach the decode.** That earlier suspicion was wrong: there was
simply nothing in the band for it to act on in the frames being measured. The
28 px the pass produced on row 110 sit at x=112..141 -- the vehicle at guest
x 16..45, nowhere near the edge. "0 px difference" was measuring an empty
margin, not a broken hint.

### CLOSED: the game culls its sprites at the view edge

`SC_OAM_TRACK=1` logs every on-screen OAM slot near the right edge, every
frame. Across a 240-frame pan, of 43 slots seen there:

* **not one** slot's X ever moves through 256;
* the only two that move at all reach a maximum X of **252** and 219;
* slot 109 (tile 111) walks smoothly right at 4 px a frame -- 200, 204, 208
  ... 248, **252** -- and then simply stops existing. It is removed from OAM.

That is the game culling. A sprite is dropped as soon as it passes the edge of
the 256 px view, so there is nothing in OAM for a widened margin to draw,
whatever the decode does and whatever the compositor samples.

**So this cannot be fixed by decode or compositing, and both attempts at it
were sound code aimed at the wrong layer.** Showing the locomotive in the
margin would mean synthesising it host-side from the game's own vehicle state,
the way the map is already drawn host-side -- a far larger feature than a
rendering fix, and one that would have to find and read that state first.

**Do not repeat this test badly.** `SNESRECOMP_LAYER_MASK` cannot be used to
ask whether something is a sprite: the host's own per-line passes
(`s_ws_bg_margins`, `s_ws_obj_clip`) write `g_snes_ppu_dbg_layer_mask` every
line and clobber it, so the env reads as having no effect. Isolate a layer with
a scratch-buffer pass instead.

## 3. Loan view broken in widescreen -- FIXED

`savestate_7.bin`. The bank scene showed city terrain smeared across both
margins.

It was the host map, drawing exactly what it was told to.
`host_map_screen_live()` accepted the loan screen as the city view, because its
BG2 test allows the map on **either** screen and the bank puts BG2 on the
subscreen. Two things then followed: the host map painted terrain into the
margins, and the margin blank was skipped, since that is guarded off whenever
this function says yes -- so nothing could clean up after it.

Enable bits separate the three cases that reach that test:

| screen | main | sub | |
|---|---|---|---|
| city | `17` BG1\|BG2\|BG3\|OBJ | `04` | BG2 on MAIN |
| advice | `14` BG3\|OBJ | `03` | BG2 sub, BG1 sub |
| loan | `15` BG1\|BG3\|OBJ | `02` | BG2 sub, **BG1 MAIN** |

So the map on the subscreen only, while BG1 holds the main screen, means the
picture belongs to that BG1 scene and the city is merely showing through colour
math. Testing "BG2 on main" instead would also have caught the loan screen --
and would have dropped the advice page with it, whose dimmed city in the
margins is wanted.

With the host map out of the way the existing machinery does the rest: the
margins are blanked and then filled with the screen's own sky colour, which is
the "borders coloured like History or TAX" that was asked for.

Verified: savestates 1, 2, 3, 5, 8 and 9 are pixel-identical, and so is the
advice page with the panel open.

## 4. Mouse pointer off-spot after refocusing -- FIXED

`SDL_GetRelativeMouseState()` reports movement since the last call and keeps
accumulating while the window is unfocused or the pointer is outside it. Alt-tab
away, move across the desktop, come back, and the next call returns that whole
journey as one delta, so the cursor jumps far from the pointer.

The F3 toggle already discarded the stale delta for exactly this reason; the
same discard now happens on regaining focus or the pointer re-entering, polled
from `SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS` rather than handled as an
event -- those two flag names are spelled the same in SDL2 and SDL3 while the
events are not, and this file has been bitten three times by SDL renames that
still compile.

Needs confirming in play; it cannot be exercised headlessly.

## 5. Widescreen colours break when the mouse is used inside the menu
`savestate_8.bin`

Colour corruption tied to mouse input on a menu screen. Given the compositor
derives its subtrahend per frame from a modal host-vs-guest difference, check
whether the menu plus pointer defeats that estimator -- `SC_EXT_SUB=0` and
`SC_DIM_PROBE=1` will say quickly whether it is that or something else.

## 6. Mouse does not work on menus -- FIXED (needs confirming in play)

`savestate_9.bin` (INFORMATION menu), and reported again as: the mouse does not
activate buttons in the tax menu, the last d-pad choice stays selected; the
mouse does not work on normal menus; **and it does work while a button is
held**.

That last report is the one that solved it. Two paths move the cursor:

* `apply_mouse_delta()` pokes `$01eb`/`$01ed` whenever the pointer moves. That
  is what works in the city view.
* a synthesised d-pad, `s_mouse_dir`, which was consumed **only while the LEFT
  button was held**.

The menu pages track their own selection and do not take it from `$01eb`, so
the poke does nothing there and the direction is the only thing they react to
-- which is exactly why holding a button made the mouse work and letting go
made it stop.

The direction is now fed on menu pages without a button. Not unconditionally:
in the city view the poke already moves the cursor, so feeding the d-pad there
as well would move it twice per frame. `host_map_screen_live()` is the
discriminator, and it is the existing one -- false for exactly the pages that
report `$14 == 0` without BG2 enabled, which are the menu pages.

Confirmed along the way, and worth keeping: the game does **not** rewrite
`$01eb` while a menu idles (zero writes over 70 frames), so the poke was never
being overwritten -- it was simply being ignored.

## 7. Colour repeats to the border on the tax menu -- FIXED

`savestate_8.bin` reproduces it **standing still**, no mouse needed. The cursor
was a red herring throughout: it only supplied whichever colour got repeated,
which is why it looked like a black bar when its last pixel was black.

Found by snapshotting one margin pixel through the end-of-frame path:

    before_all              row45[400]=000000
    after_hide_furniture    row45[400]=000000
    after_fill_flat         row45[400]=ffdeb5   <- here
    after_fill_margins      row45[400]=ffdeb5

`ws_fill_flat_margins()` extends a flat background into the margin using **each
row's own edge pixel**. On the tax menu the panel very nearly touches the
guest's right edge, so rows 41..53 -- the TAX RATE row -- painted the panel's
colour across the whole margin.

Fixed by splitting the two jobs the edge pixel was doing: the flat run still
qualifies a row using that row's own edge, and the colour painted is now the
one the majority of rows agree on (`bg_edge`, 213 of 224 votes here). Those 576
stray pixels become background; the black count is unchanged; and savestates
1, 2, 5, 7 and 9 are all pixel-identical.

Getting there ruled out, each by measurement: sprites (`SC_WS_OBJ_CLIP` 0 vs 1,
0 px), the `s_ws_bg_margins` copy (instrumented, does not run here), the
per-line margin blank (runs on all 224 lines and leaves black), and a per-line
clamp (`s_ws_clamp_now` has one write site, per frame).

One wrong turn worth keeping: testing flatness against `bg_edge` too. A row
whose edge is the panel is perfectly flat, just not in the background colour,
so it failed the test, was skipped, and kept the blank's black -- trading a
coloured stripe for a black one. The run decides WHETHER a row is
background-like; `bg_edge` decides WHAT to paint.


A frame-level clamp of `0f` cannot explain 13 specific lines. So the clamp
state almost certainly varies **per line** -- HDMA windowing on that row -- and
the blank skips exactly those lines, letting the BG tilemap wrap into the
margin. `SC_CLAMP_DIAG` was added for one run and reverted; the next step is to
sample `s_ws_clamp_now` per scanline across y=35..60 rather than once a frame,
and if it dips there, decide whether the blank should key off something other
than a full clamp.

## 8. Selector pins and win marks missing on the outer columns -- CLOSED

Reported 2026-09-05: on the scenario selector the pins AND the win marks are
missing on the left and right columns. Same root cause as the locomotive, and
now confirmed directly on that screen.

Dumping OAM on `savestate_2.bin` (`SC_OAM_BAND=2`), the pin sprites -- tiles
142/143/175/185 on the two card rows -- exist at guest x = **120, 200, 216
only**, all inside the 256 px view, plus one parked at -40. Nothing is emitted
for the columns widescreen reveals. The game culls its sprites to its own view,
exactly as the OAM tracker showed for the train.

**Not mine, and not the classifier change.** Rebuilt against the previous
submodule commit (`1d9cd45`, before the step test) and rendered the same state:
**0 px difference**. The outer pins were already absent.

So the only way to show them is to synthesise them host-side -- the pin colours
and the win flags at `$700007` are both readable -- which is the same feature
the locomotive would need, and a much larger job than a rendering fix.

## 9. Loan -> map transition: margins flash before the black -- FIXED

Leaving the loan screen, the guest faded 125.8 -> 8.5 with the margins tracking
it, then the frame the guest reached 0 the margins snapped to 189.3 -- full
brightness -- and held it for twelve frames.

The per-line margin blank was painting it. It writes the backdrop through
`brightnessMult`, and SimCity ends a fade by writing `$8f`: force blank on,
**brightness restored to 15**. So it computed `cgram[0]` at full intensity and
painted the sky into the margins while the guest was black. The PPU had already
blanked the line correctly; this painted over it.

Probing one margin pixel through a single line said it outright:

    after_runLine        row100[400]=000000  blank=1
    after_margin_blank   row100[400]=adbdce  blank=1

Fixed by painting black when `PPU_forcedBlank()` -- the same test the
compositor already uses, on the path that runs when the compositor does not.
Margins now reach 0.0 on the same frame the guest does. Savestates 1, 2, 3, 5,
7, 8 and 9 are pixel-identical at rest.

**Two false trails, both worth recording.** An earlier attempt added
`PPU_forcedBlank()` to this block's *condition* rather than to the colour it
writes -- so it still painted bright sky, and "changed nothing" looked like
evidence that force blank was not involved. It was.

And the diagnostics that "proved" the block was never reached were gated on
`s_frames`, which **`load_state()` restores from the save file** -- it starts
around 31196, not 0, so no frame gate ever matched. Dump filenames use a
separate run-relative counter. Gate probes on a counter you increment yourself.

## CORRECTION (2026-09-05): the "game culls sprites" finding was wrong

Reports 2 and 8 -- the locomotive, and the selector's outer pins and win marks
-- were closed on the grounds that the sprites do not exist beyond the guest's
view. **That was wrong, and the cause was a bug in my own diagnostic.**

`SC_OAM_BAND` and `SC_OAM_TRACK` decoded the 9th X bit as
`highOam[i >> 3] >> ((i & 7) * 2)`. High OAM packs **four** sprites per byte,
two bits each, so it is `highOam[i >> 2] >> ((i & 3) * 2)`. Read the wrong way
the shift runs off the end of the byte and bit 8 comes back as zero for most
slots -- so every sprite looked confined to x < 256, which is exactly the
evidence I used to declare culling.

With the decode fixed, on the same pan of the same save state:

| slot | X range | tile |
|---|---|---|
| 39 | 198..384 | 0 |
| 109 | **256..396** | 111 |
| 123 | 236..260 | 78 |

Slot 109 is the mover the tracker followed. It goes to **396**, well into the
margin. The objects are there.

The user said from the start that the train and the pins are missing "only in
widescreen". That was right, and my measurement was what was wrong.

### What actually hides them

Two layers, and both have to be dealt with:

1. **The decode.** `PpuDecodeOamX` wraps `x >= 256 + extraRight` unconditionally,
   and wraps `[256, 256+extraRight)` too unless the slot is hinted. Slot 109
   spends part of its run beyond 352, where nothing but a wider `extraRight`
   can help, and part inside the band, where a hint would.
2. **The compositor.** In the city view `host_map_compose()` fills
   `dst[256..447]` from the host map, which draws BG tiles only, so even a
   correctly decoded sprite is painted over. That is why the right-hints and
   the OBJ margin pass each measured 0 px *individually* -- and why testing
   them together still failed, since the part of the run beyond 352 stays
   wrapped regardless.

### Also wrong, and now reverted

The `01:f13b` carry path was read as the cull and a hook written to defer it.
It makes no measurable difference (`SC_WS_OBJ_MARGIN` 0 vs 1: identical slot
ranges), because that carry is not what removes these objects from view. The
hook is reverted; only the decode fix is kept.

The `01:f11a` analysis in `ROM_MAP.md` still stands as a description of the
routine -- an 8-bit X stepped per frame, recycled via `$00c22c` on carry -- but
its conclusion, that this is why objects vanish at the edge, does not.

## Moving objects now cross the right margin -- FIXED

Both halves were needed, which is why each measured 0 px alone:

1. **The decode.** `PpuDecodeOamX` wraps the ambiguous band unless the slot is
   hinted, so the traffic was thrown away before anything could draw it.
2. **The compositor.** `host_map_compose()` fills the margin from a BG-only
   host render, so a correctly decoded sprite was painted over anyway. An
   OBJ-only pass (`g_snes_ppu_dbg_layer_mask = 0x10`) into a scratch buffer now
   supplies those columns.

Measured on `savestate_3` while panning: the object steps 4 px a frame from
x=260 to x=349 across frames 94..116 -- exactly the window where slot 109 holds
raw X 256..348 -- where before there was nothing.

### The hint has to be per slot

Hinting the whole band was too coarse. The city view parks HUD sprites in it as
well, and a blanket hint decoded them positive and printed the date, "1902 JA",
across the right margin of `savestate_9`.

The classifier already separates them: traffic steps 4 px a frame and carries
motion grace, parked HUD text never moves. Hinting only slots with
`wsOamMotionGrace` keeps the boat and drops the HUD -- verified both ways, the
object still appears and `savestate_9` is pixel-identical again.

Verified: the guest's own columns change by **0 px**, and savestates 1, 2, 3, 5,
7, 8 and 9 all render pixel-identical at rest.

`SC_WS_CITY_OAM_RIGHT=0` and `SC_WS_MARGIN_OBJ=0` disable the two halves
independently.

**Note on the far end.** Beyond raw X 352 the decode is unconditional -- those
values mean negative positions on hardware -- so an object leaving to the right
still disappears once it passes 351 rather than running off the true edge. That
is a separate limit and is not addressed here.

## Overview / Vote / History: the border went black in one frame -- FIXED

Reported from play: dismissing the city overview, the green border is "drawn to
black too fast", while the fade back into the map is on point.

`ws_fill_flat_margins()` was gated on `ws_display_settled()`, which demands
`brightness == 0x0f`. So the moment a fade starts the fill stops running and the
margins keep the per-line blank's black, while the guest dims gradually.
Measured on `savestate_4` (dismiss with X -- the pad bit, not the physical
label): the panel walks 216 -> 15 over fourteen frames while the border drops
49 -> 0 in **one**.

Nothing in that function needs full brightness. The colour it paints is the
guest's own edge pixel, which the PPU has already dimmed by the same amount, so
running it during a fade makes the margin track for free. Force blank still bars
it -- there is genuinely nothing to show then, and the per-line blank owns the
margins.

After the change the border/panel ratio holds at **0.21 for every frame** of the
fade and both reach 0 together. All eight save states are pixel-identical at
rest.
