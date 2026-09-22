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

**FIXED 2026-09-22** by `src/sc_vehicles.c`: the game drops every object
sprite at `00:c019` once it lies right of the view, so the host keeps what
it drops and draws it in the margin. See docs/ROM_MAP.md, "`01:f11a`".
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

**FIXED 2026-09-22.** The game does emit the margin cards' pins and marks;
the pins sit in the ambiguous band, the marks decode negative, and the
strict decode hid both. They are now claimed by exact position, each sprite
recomputed from records `$12` (pins) and `$29` (marks) and matched against
OAM (`selector_hint_margin_sprites()`); the hidden bracket is left alone.
The earlier mark matcher looked at each record's base, where none of its
four sprites sits, and never matched.

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
`brightnessMult`, and the game ends a fade by writing `$8f`: force blank on,
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

## The train shows one tile then vanishes -- FIXED 2026-09-22

**FIXED 2026-09-22** by `src/sc_vehicles.c`: the game drops every object
sprite at `00:c019` once it lies right of the view, so the host keeps what
it drops and draws it in the margin. See docs/ROM_MAP.md, "`01:f11a`".

Reported from play: on the normal map the train is fine, then in the margin
"it is shown just one tile and then it disappears suddenly".

Both halves of that are now explained, and one of my guesses is disproved.

**It really is one tile.** Tracking OAM across a pan, slots 100-108 sit
permanently parked at x=384 and only **slot 109 moves**. The object is a single
sprite, so "just one tile" is not a rendering fault -- that is the whole object.
My guess that this and the intro sign were one fault, multi-sprite objects
losing members individually, is wrong for the train at least.

**The sudden disappearance is an architectural mismatch**, not a decode or
compositing bug:

* the composited picture is 448 px -- the guest's 256 columns at `dst[0..255]`,
  then **192 px** of host map at `dst[256..447]`;
* the PPU renders `extraRight = 96`, which covers only `dst[256..351]`.

So beyond dst 351 there is no PPU output to composite at all, and the sprite
stops dead halfway across the visible margin. That is also why the margin OBJ
pass measured a clean crossing from x=260 to x=349 and then nothing.

**The route, and why it is not a one-liner.** The runner already has both
pieces: `kPpuExtraLeftRight` is now **272** (upstream raised it for ultrawide),
and `PpuSetExtraSideSpace(left, right, bottom)` sets an asymmetric per-side
margin within that budget. Keeping `extraLeft = 96` preserves every existing
offset (the compositor reads the guest at `gst + s_ws_extra`), so asking for
`right = 192` would give the PPU coverage across the whole strip.

What blocks it is buffer width. The PPU would render 96 + 256 + 192 = **544 px**
per line into buffers sized from `kVideoWidthMax = 256 + 96*2 = 448`, and
`PpuBeginDrawing` is handed `s_video_pitch` for the frame, the OBJ layer and
both scratch surfaces. All of those have to grow together, and `main.c` still
clamps `SC_WIDESCREEN` to 96 with a comment claiming that is the runner's cap --
which is now stale.

## The motion classifier only ever saw dx = 0 -- fixed

The widescreen margins admit an unhinted moving sprite on the strength of
`wsOamMotionGrace`, a per-slot countdown the PPU refreshes each frame when the
slot's X has stepped. Measured on the city view, that countdown was never alive
for longer than a single frame, so no game-authored object could hold one:

| | longest unbroken run of frames with any slot in grace | frames with grace |
|---|---|---|
| before | **1** | 119 of 896 |
| after | **68** | 475 of 897 |

A moving sprite needs grace on EVERY frame to stay drawn in a margin, so at a
run length of 1 the answer was effectively always no. That is the train the
report describes, and the same countdown feeds `s_oam_right_hints`, so the city
view's right margin was starved by the same fault.

**The cause is a frame-boundary test that a multi-pass host defeats.**
`PpuUpdateWidescreenOamHistory` guarded its body with "skip unless the line
number stopped advancing", on the reasoning that lines rise 1..224 within a
frame and only wrap between them. True for a host that renders each line once.
This host does not: the city view re-renders every line into scratch surfaces
to isolate the backgrounds and the OBJ layer, and the second pass over a line
arrives with `line == lastLine` -- which the old test read as a new frame. The
body ran on the order of once per line instead of once per frame, and since OAM
does not change between two passes over the same line, every repeat saw
`dx == 0`: the first pass set the grace, the repeats decremented it straight
back to zero. Directly observed, the stored previous X equalled the current X
on every single frame.

The fix tests for the line number going BACKWARDS, which a repeated line never
does. All nine save states are pixel-identical at rest.

### Two things this cost, recorded so they are not repeated

**The instrument caused the symptom it measured.** The probe built to watch this
armed an extra OBJ-isolation render pass, which on the title -- where no margin
pass runs and each line is otherwise rendered once -- created the very
double-render that breaks the classifier. Every title measurement in this chase
is therefore suspect. The result above is from the city view with no probe pass
armed, which is why it is the one quoted.

**A build that did not rebuild.** Two rounds of "baseline vs fixed" reported 0
pixels different because both dumps came from the same binary. Hash the
executable between the two builds; do not trust an empty error grep.

## Known bug: the title sign in the left margin

Not fixed, and deliberately left. The sign travels out through the left margin
correctly, but it then PARKS at x = -32 for about 1026 frames before the
sequence repeats. Hardware clips it; a 96 px margin does not, so it would sit
visible against the left border for the best part of twenty seconds -- which is
the "sticking to the left border for a whole round" already reported from play
and already reverted once.

The margin gate hides a parked object once its grace expires, which is what
stops that. The cost is that the sign fades rather than leaving cleanly. Both
behaviours come from the same rule and no measurement taken here separates a
sign that has finished its travel from one that is merely between steps.

Anything further needs the title's own sequence data rather than a heuristic --
the ROM knows when the sign's move ends, and the classifier can only guess.

## The ninth scenario had no win/lose rules at all -- fixed

Reported from play: "Sylt is losing after a short time." It was losing every
time, immediately and unavoidably.

Every per-scenario table the ROM indexes with `$0040` holds EIGHT entries, and
they sit back to back, so index 8 reads the first entry of whatever table
follows. `03:cec8` already repaired the seed. Three more were still short:

| table | what index 8 read | fix |
|---|---|---|
| deadline year `$03c5b3` | `5`, the first entry of the countdown table | 2057 |
| objective ladder `03:c557`+ | ran `Y = 0..6`, fell through writing NO result | a rule of its own, below |
| win-mark mask `$03e334` | `0xbb22` | bit 8 |

**How the loss happened.** `03:c502` computes deadline minus `$0b53` and walks
`$0ccb` down a 5,4,3,2,1,0 countdown, evaluating the objective at the end.
Against a deadline of 5 and a start year of 2047 the `SBC` borrows, `03:c51e`
clamps the remainder to 0, and all six countdown steps are consumed in six
calls. `$0deb` is 1 there against the `CMP #$0004` at `03:c54b`, so the verdict
was always a loss. Confirmed in play afterwards: `$3e=3 $40=8 year=2047
A=0809`, `$0ccb` steady at 0 across 59 samples, `$0d87` never written.

The win-mark table matters even though nothing could win before: with the
objective repaired, `0xbb22` would have gone to SRAM `$700007` via `03:e326`,
scattering marks across scenarios never played and setting bit 15, the game's
own "all six beaten" flag. Bit 8 is free -- the six scenarios own 0-5, Las
Vegas and free play 6-7.

### Sylt shows no win mark -- FIXED 2026-09-22

Sylt's card now carries a pin (Rio's colour, as Sylt takes Rio's entries)
and, with bit 8 of `$42` set, record `$29` one column right of Las Vegas's
mark, both placed by `selector_sylt_sprites()` in parked slots. Checked in
the widescreen at scroll `$50` and `$A0` and at native width.

The fades, reported from play right after: going to the fax and back, the
margin pins and marks vanished. The selector is on screen for `$14` = `$0A`
(fade-in), `$0B` and `$0C` (fade-out to the fax), and everything was gated
on `$0B`. With that widened, the fade-in still showed the shipped map --
black on the right, no Sylt card, Sylt's pin and mark over the black --
because the wood extension and the cards were made at `03:ddb6`, after the
fade. They are now also made on `$0A` once the selector's one big VRAM DMA
has landed (`selector_after_upload()`), so the whole selector fades in
together; this also closes the Sylt card popping in after the fade.

The original note:

Not fixed, by decision. The drawer at `03:ded0` walks exactly eight bits with
two eight-entry coordinate tables (`$03df20`, `$03df30`), so bit 8 has no
coordinates and paints nothing. Showing it means synthesising the sprite host
side, which needs the mark's tile and palette; `SC_MARK_DIAG=1` prints both
from a real mark the next time one passes the selector, so that groundwork is
done. The win itself records correctly in SRAM -- only the card is unmarked.

Also still unverified: that Sylt can be WON. The premature loss is measured
fixed; reaching 2057 at city class 4 has not been played through.

## The city name -- fixed

Reported from play: Sylt was called PRACTICE on the fast-travel minimap and in
view mode. Nothing was corrupt. `03:cf19` copies a length-prefixed name to
`$0b5b` from a pointer table at `$03cf32`, and that table is one of the few
indexed by `$0040` that is NOT short: it has a real ninth entry, and the ninth
entry is the practice map's own name, which index 8 legitimately is.

| idx | 0-5 | 6 | 7 | 8 |
|---|---|---|---|---|
| | CISCO BERN TOKYO DETROIT BOSTON RIO | LASVEGAS | FREEDOM | **PRACTICE** |

Rewritten in place at `03:cf31`, in the `A = 0x0a` alphabet the briefing writer
already uses. One stored name feeds both the minimap and view mode, so both
follow.

**The gate took two attempts, and the first was wrong.** Index 8 is also the
tutorial, so the rename has to distinguish them. The latch was cleared only at
`03:ddb6`, the selector -- which the tutorial never reaches, because it starts
from the main menu. A latch left set by a Sylt session survived into the next
practice map and renamed it: "now the Practice map shows Sylt". It is now
cleared at `03:ce2e`, the map-loader entry BOTH maps pass through, and set
again at the swap, so it describes the load in progress and nothing earlier --
and it does so whichever order the seed and the swap run in.

## The selector's marks are not a job for the motion classifier -- fixed

Reported from play once the classifier started working: on the selector the red
marks "are visible only when the screen is moving, not when it stands still",
and the parked green bracket "gets back with small fragments" during a scroll.

Both are the temporal fallback doing exactly what it says. `03:ded0` places
each mark at `$df30,Y` MINUS the smooth-scroll `$16`; at the ninth column `$16`
is `$50`, so bits 0 and 3 -- column 0, San Francisco and Detroit -- land at
`14 - 80 = -66`, inside the left margin. Genuine margin content, admitted while
its X changed and dropped when it stopped.

A heuristic is the wrong tool on a screen the host can enumerate. The runner
gained `wsOamMotionGraceOn`; the selector turns it off and hints the marks by
name instead, their positions recomputed from the ROM's own two tables and the
live scroll and matched against OAM. Nothing else in the margins is claimed,
so the bracket stays out.

### What Sylt's win condition should be -- decided

The full set, decompiled from `03:c548`:

| idx | scenario | start -> deadline | objective |
|---|---|---|---|
| 0 | San Francisco | 1906 -> 1911 | *(nothing further)* |
| 1 | Bern | 1965 -> 1975 | traffic `$0c05` **< 80** |
| 2 | Tokyo | 1961 -> 1966 | score `$0ded` **>= 500** |
| 3 | Detroit | 1972 -> 1982 | crime `$0c01` **< 60** |
| 4 | Boston | 2010 -> 2015 | score **>= 500** |
| 5 | Rio | 2047 -> 2057 | score **>= 500** |
| 6 | Las Vegas | 2096 -> 2106 | *(nothing further)* |
| 7 | free play | -- | never judged |

`$0ded` is the city score, initialised to exactly `#$01f4` = 500 at `03:b485`
in the block that clears the evaluation counters, so ">= 500" means back to
where it started. `$0c01`/`$0c05` are two of the four statistics copied
together to the evaluation page at `03:b582`, and they line up with Detroit's
and Bern's themes.

**The gate none of the entries above mentions.** `03:c54b` tests `$0deb >= 4`
before any per-scenario objective is reached, and the class ladder at
`03:81d8` prices class 4 at **100,000 inhabitants**:

| class | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| people | <2k | 2k | 10k | 50k | **100k** | 500k |

So every stock scenario secretly requires 100k. Sylt starts at 3,400 on a
small island -- the real one holds about 18,000 -- so under that gate it could
not have been won at all whatever objective it was given, and adding a score
test would only have been strictly harder, the class gate coming first either
way.

**Decided: score >= 500 and city class >= 2, within the ten years already
set (2047 -> 2057).** Both halves are the ROM's own measures rather than
invented ones: the score bar three stock scenarios already use, and a size
floor of 10,000 that an island can plausibly reach. Implemented at `03:c548`,
ahead of the class gate, jumping to the ROM's own `c5a7`/`c5ac` so the result
is still stored in one place. The earlier hook that routed index 8 into the
ladder alongside scenarios 0 and 6 is gone -- it sat *after* the class gate and
so could never have fired.
