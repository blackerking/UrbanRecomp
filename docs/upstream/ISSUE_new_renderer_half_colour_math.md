# The new renderer drops background rectangles on halved colour math when extra space is non-zero

**FILED: https://github.com/mstan/snesrecomp/issues/30**

> **Written by an AI.** This was researched, measured and written by Claude
> (Anthropic), working on a SimCity SNES recomp host with @blackerking, who has
> reviewed and approved posting it. Every number below is from an actual run
> against this repo, but the reasoning is mine and worth sanity-checking.

Related to #25 (closed), which is the converse of this — see "The pinch" below.

## What happens

On a screen whose picture is **backdrop plus a halved subscreen**
(`cgadsub=$60` → math on the backdrop only, `half=1`, add; `cgwsel=$02` →
`addSubscreen=1`), `kPpuRenderFlags_NewRenderer` loses whole rectangles of the
background as soon as `PpuSetExtraSpace()` is non-zero.

In SimCity this is the advisor popup: a panel over a dimmed city. With the new
renderer, bands of the city above and below the panel come out as backdrop. A
256-wide render of the identical save state shows ordinary city there.

## Measurement

Same state, same frame, comparing only the authentic 256 columns against a
plain 256-wide render (`SC_WIDESCREEN=0`):

| renderer | authentic pixels wrong |
|---|---|
| `ppu_draw_whole_line_legacy` | **0** of 57344 |
| `PpuDrawWholeLine` (new) | **8736** of 57344 |

It is not a geometry effect. Varying only the extra space:

| extra space per side | frame width | pixels wrong |
|---|---|---|
| 8 px | 272 | 8736 |
| 32 px | 320 | 8736 |
| 96 px | 448 | 8736 |

Identical count at every width, so any non-zero extra space switches the
behaviour and the amount is irrelevant. At `SC_WIDESCREEN=0` the two renderers
agree exactly.

## Scope

Across ten save states covering every screen in the game, comparing the
authentic 256 columns at 448 px wide against a 256-wide render of the same
state:

| screen | `cgadsub` | pixels wrong |
|---|---|---|
| city view | `$b3` (subtract) | 0 |
| stats pages | `$20` (add, not halved) | 0 |
| map select, name entry, menus | — | 0 |
| **advisor popup** | **`$60` (add, halved)** | **8736** |
| View Mode | `$00` (no math at all) | 1049 |

The halved-math screen is by far the worst, but **View Mode is doing no colour
math at all and is still wrong by 1049 pixels**, so there may be more than one
path affected here. I have not chased that one.

## The pinch, with #25

#25 established that the `ws*` layer policies are no-ops on the legacy
renderer, so a host that wants clamping or mirroring must select the new one.
This issue says the new renderer is wrong on halved colour math whenever extra
space is set. On a screen that needs both, there is currently no correct
choice.

In practice it is survivable: a screen dimming a scene behind an overlay tends
to have every background clamped anyway, so there are no policies left to
apply and dropping to legacy costs nothing. That is what this host now does —
select the legacy renderer when `PPU_halfColor(ppu) && PPU_addSubscreen(ppu)` —
and it takes the advisor popup from 8736 wrong pixels to 0. It is a workaround
for a host that knows the game, not a fix.

## Reproduction

Any content composing backdrop plus a halved subscreen, rendered twice — once
with `PpuSetExtraSpace(ppu, 0)` and once non-zero — comparing the authentic 256
columns. The difference appears as rectangles of background replaced by
backdrop.

The SimCity save state that shows it cannot be attached: it carries ROM-derived
WRAM and VRAM, and that project is deliberately ROM-free. Happy to run any
instrumented build against it and report back, if that helps more than a
synthetic case.
