# HDMA per-scanline execution was entirely missing (resolved)

Status: **fixed**, in `src/main.c` (game-specific, not the shared
`snesrecomp` submodule -- see "Where the fix lives" below).

## Symptom

The View screen (the watch icon in the game menu) is supposed to show the
city map as a tilted, perspective-skewed "photograph on a table", per a
real screenshot from this ROM. This recomp instead rendered it completely
flat, with black letterboxing where the wood-table background should be.
As a bonus, fixing this also resolved a separate, previously-unreported
"printer" bug the same way.

## Root cause

Checked first via a temporary `SC_GFX_TRACE` tool (see `src/main.c`,
env-gated, watches CPU-issued writes to BGMODE/$2105, the Mode 7
matrix/center regs $211b-$2114, HDMAEN/$420c, and every HDMA channel's
config regs $43x0-$43xa): the View screen never touches Mode 7 at all
(`$2105` is written `01` -- plain BG mode -- every frame, and there are
zero writes anywhere to `$211b-$2114`). Instead, three HDMA channels
(5, 6, 7; enabled together via `$420c=e0`) are configured to update, once
per scanline, the window-boundary registers `$2126-$2129` (WH0-3) and the
BG1/BG2 horizontal-scroll registers `$210d`/`$210f` -- the classic
"fake-3D via per-line window + scroll" trick, not real Mode 7 rotation.

Tracing further into the shared engine (`snesrecomp/runner/src/snes/dma.c`)
found the actual bug: `dma_write`'s `$420c` handler correctly sets each
channel's `hdmaActive` flag, but **nothing anywhere in the cycle-accurate
DMA path ever reads that flag to perform a transfer.** `dma_doDma`/
`dma_cycle` only implement plain DMA (`$420b`-triggered, `dmaActive`).
A complete, correct per-line HDMA table-walk implementation *does* exist in
the shared runtime -- `SimpleHdma_Init`/`SimpleHdma_DoLine` in
`snesrecomp/runner/src/common_rtl.c` -- but it's written for the
AOT/decompiled recomp path (games with actual decompiled C functions
calling it inline) and nothing in this interpreter-only project ever calls
it. A stray `dma_startDma(snes->dma, 0, true)` call, once per frame at
`vPos==0` in `handle_pos_stuff`, made this worse: it unconditionally zeroed
every channel's `hdmaActive` regardless of what the game's own `$420c`
write had just set, undoing the one piece of state the (missing) execution
code would have needed anyway.

Net effect: **HDMA execution was a complete no-op in this project's
execution mode.** Any HDMA-driven effect in this ROM -- not just the View
screen's tilt, evidently also whatever the "printer" feature uses -- simply
never applied.

## The fix

Ported the same per-line HDMA algorithm into `src/main.c` as
`hdma_init_channel`/`hdma_do_line`, using this project's own
`snes_read`/`snes_writeBBus` bus primitives (the same ones `dma.c`'s own
plain-DMA path already uses in `dma_transferByte`) instead of
`common_rtl.c`'s raw host pointers -- this also sidesteps a real linkage
problem: `common_rtl.c` defines its own `uint8 g_ram[0x20000]`, which would
collide with `src/main.c`'s own `g_ram` definition if the file were linked
in directly.

Wired into `handle_pos_stuff` at the same two points the shared driver
already handles plain DMA:
- Once per frame, at `vPos==0`: replaced the harmful `dma_startDma(dma, 0,
  true)` reset with `hdma_init_channel` for all 8 channels, latching each
  currently-enabled channel's table pointer from `hdmaActive` as the game
  left it.
- Once per scanline, at the existing `hPos==1024` / `!inVblank` branch
  (right where `dma_cycle` already continues plain DMA): added
  `hdma_do_line` for all 8 channels. This timing matters -- it fires during
  the *current* line's hblank, so the register values it writes take
  effect starting with the *next* line's render at this same loop's
  `hPos==0` branch, matching real hardware.

Verified: `--qualify 300` passes cleanly (no regressions to boot/attract
behavior), and interactive testing confirmed the View screen's tilt effect
now renders correctly.

## Where the fix lives

Entirely in `src/main.c` (game-specific). No changes to the shared
`snesrecomp` submodule were needed -- the existing plain-DMA machinery in
`snes/dma.c` (channel config, `hdmaActive` tracking) was already sufficient
raw material; only the per-line execution step was missing, and that could
be added game-side using primitives the submodule already exposes.

## Caveat: other games sharing this engine

This gap is almost certainly not specific to this game -- any other
interpreter-only (non-AOT) recomp project built on this same `snesrecomp`
checkout would have the identical no-op HDMA problem for any ROM that uses
HDMA for anything (fades, gradients, split-scroll, window effects, etc.).
Worth flagging upstream or porting the same fix if/when another
interpreter-mode project on this engine needs it.
