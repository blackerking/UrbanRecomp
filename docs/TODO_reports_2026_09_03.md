# Reported from play, not yet investigated

Six items reported 2026-09-03. Save states are on disk in the current format
and load on this build; they are NOT in the repo (ROM-free), so they must be
kept locally.

## 1. Scenario selector: blinking cursor at the left, pins still missing
`savestate_2.bin`

Two symptoms on one screen. The missing won-marks are already understood and
recorded at the OAM-hint block in `main.c`: the marks for the widescreen-only
columns are not in OAM at all, so no decode change can bring them back.

The **blinking cursor on the left** is new and is a real lead. While testing a
blanket left-hint release, exactly such an artefact appeared -- a stray green
selection bracket at x=16..71, drawn from slots 2 and 11 (tiles 76/78 with flip
bits `f4`/`b4`), a second bracket the game parks off-screen-left. That
experiment was reverted, so if a blinking cursor is visible NOW it is either a
different object or something reaches the margin by another route. Start by
dumping OAM on the screen (`SC_OAM_BAND=2`) and comparing against the reverted
experiment's capture.

## 2. Locomotive not drawn in the widescreen margins
`savestate_3.bin` -- the state shows it as it appears in the normal view.

A small moving object that renders in the guest's own columns but not in the
margins. Almost certainly the same class as the title sign: an unhinted OBJ,
gated by the strict left/right decode or by `wsOamMotionGrace` expiring. Check
whether it is in the ambiguous band, and whether upstream's motion classifier
is holding it (`kPpuWsOamMovingGraceFrames` is 4 frames of unchanged X).

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
