# Widescreen has no left-band counterpart to `wsOamRightHint`

**FILED: https://github.com/mstan/snesrecomp/issues/28**

`origin/main` @ `fe6045c`.

## The asymmetry

For the right-hand margin the PPU already has a way to tell a wanted sprite
from a parked one. `ppu.h`:

```c
// Strict decode of the ambiguous 9-bit OAM X band [256, 256+extraRightCur).
// A raw value there is either a genuine right-margin sprite (widescreen
// host emitted it on purpose) or a sprite the game parked off-screen-left
// at x-512 (invisible on hardware). When strict is set, only slots marked
// in wsOamRightHint keep the positive decode; unmarked slots wrap negative
// like hardware, so parked sprites don't ghost into the right margin.
uint8_t wsOamRightHintStrict;
uint8_t wsOamRightHint[16];
```

The **left** margin has the mirror-image problem and no mirror-image control.
A sprite at negative X is either

* a sprite the game parked off-screen-left to hide it — hardware clips it, and
  a widescreen host should keep clipping it; or
* a sprite that is genuinely wanted, because the background scrolled and the
  widened view now shows the region it belongs to.

Nothing in OAM separates the two, and there is no `wsOamLeftHint` to say which.

## What it looks like

This game's scenario selector blinks its green selection cursor by toggling a
flag and skipping the draw — and it hides the sprite by **parking it at a
negative X**. Measured on one save state, identical frames at both widths:

| frame | 256 wide | 448 wide (96 per side) |
|---|---|---|
| 1432, 1444 | cursor at 176..239 | cursor at 272..335 — 176+96, correct |
| 1436, 1440 | **nothing** — blink off | **box at 48..71, in the left margin** |

So the cursor appears to blink between two places: its real position on the
"on" frames, and the parked position on the "off" frames.

The same screen also has the opposite case. Its scenario cards carry red win
marks placed at `table_x - scroll`, which goes negative for any card the scroll
has pushed out of the authentic 256. Those cards are visible in a widened view,
and their marks are wanted.

## Why a host cannot work around it

Suppressing sprites in the margins removes both. In this game that means losing
the win marks and the selection cursor from every card sitting in a margin —
state the player needs — in exchange for hiding a cosmetic ghost. Allowing them
keeps the marks and keeps the ghost. There is no third option from outside the
PPU.

## Suggestion

A symmetric `wsOamLeftHint` / `wsOamLeftHintStrict`, set through something like
`PpuWsSetOamLeftHints()`, would let a host mark the slots it knows are genuine
and let the rest clip as hardware does — exactly the contract the right band
already has.

A cheaper variant, if per-slot bookkeeping is more than this deserves: a single
"clip fully-off-screen-left sprites" flag. It would not distinguish the two
cases, but it would at least make the choice available per host rather than
per build.

Happy to send a patch for either if one is preferred.

## Smaller note on the existing API

`PpuWsSetOamRightHints(ppu, NULL)` sets `wsOamRightHintStrict = 0`, i.e. it
*disables* strict decoding. That reads as "clear the hints" but it is easy to
call it expecting "strict with nothing marked", which is spelled by passing a
zeroed array instead. A line in the header, or splitting the strict flag into
its own setter, would remove the trap.
