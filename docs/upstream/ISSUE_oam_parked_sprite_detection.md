# Widescreen cannot tell a sprite leaving the view from one parked off-screen

**FILED: https://github.com/mstan/snesrecomp/issues/56**

*Written by an AI (Claude) working on an SNES city-builder recomp host (Urban Recomp) with
@blackerking, who has reviewed it.*

Follow-on from #28, which added `wsOamLeftHint` so a host can mark the slots it
placed itself. That control works. This is about the case it cannot express.

## The problem

A sprite at negative X is one of two things:

* **parked** — the game moved it off-screen to hide it, hardware clips it, and a
  widescreen host should keep clipping it; or
* **leaving** — it is genuinely sliding out of the view, and a widescreen host
  should draw it crossing the margin, because that is what the extra width is
  for.

Every control we have decides this **per frame, from one OAM snapshot**:
`wsOamLeftHint` marks slots the HOST placed, `wsOamLeftHintStrict` chooses the
decode, and `PpuWidescreenOamLeftHintAllows()` gates a whole sprite. None of
them can classify a sprite belonging to the GAME, because from a single frame
the two cases are identical: same slot, same negative X, same tile.

The distinction is temporal. A leaving sprite's X is changing; a parked one's
is not.

## Why this is not academic

This game's title slides a logo off the left edge. It is OBJ — six sprites,
slots 96-101, 16x16, three across by two down. Working within the existing
mechanisms, every option is wrong in a different way:

| approach | result |
|---|---|
| gate blocks it (the current default) | it vanishes 64 px before the true edge; visibly "fades out before the screenborder" |
| gate blocks, plus a per-pixel clip at x=0 | same fade, just smooth instead of stepped |
| hint the slots so the margin is admitted | it slides out correctly **and then sits in the margin** until the sequence returns for it |

The third is the interesting one: it is exactly right while the sprite moves
and exactly wrong the moment it stops. There is no per-frame predicate that
splits those, because the frame where it stops looks like the frame before it.

`PpuWidescreenOamLeftHintAllows()` refuses sprites in `-left_extra < x + size
<= 0`, which is precisely the band that is visible in the margin. That is
deliberate and correct for parked sprites, and it is what makes a smooth exit
impossible for moving ones.

## Suggested direction

Per-slot OAM position history in the PPU, and a predicate that consults it:
a slot whose X has not changed for N frames while off-screen is parked and
refused; a slot whose X is changing is leaving and admitted.

There is precedent for cross-frame state — `ws_shadow` already keeps
world-keyed history — so this need not be a new kind of thing, only a new user
of one.

## What we would want settled first

* **What counts as stopped.** A sprite that pauses mid-slide must not blink
  out. N is a tuning knob and the right value is not obvious.
* **Save-state loads.** History is meaningless across a load, and the first
  frames after one already behave differently (a host that publishes hints at
  `vPos == 0` has not published any yet). A stale history there would be worse
  than none.
* **False positives.** A legitimately stationary sprite in a margin — a HUD
  element a host placed — must not be classified as parked. Presumably the
  existing hint arrays still win, and history only decides the unmarked slots.
* **Cost.** 128 slots x a few bytes, updated once per frame, is cheap; the
  question is whether it belongs in `Ppu` or beside it.

Happy to prototype it on `blackerking/snesrecomp` if the direction is right.
We have a reproducible case and the measurements above, so we can say whether a
given rule fixes it without guessing.

## One measurement note, in case it saves someone time

Comparing a margin's total changed-pixel count between two builds does NOT
answer "is something parked there". A still object with the scene scrolling
past it produces a drifting delta that looks exactly like objects entering and
leaving. We concluded "not parked" from that and were wrong. The check has to
follow the specific slots over time.
