# SimCitySNESRecomp

Static recompilation of *SimCity* (SNES) onto [snesrecomp](https://github.com/mstan/snesrecomp),
the same general-purpose 65816-to-C framework used by
[MegaManXSNESRecomp](https://github.com/mstan/megamanxsnesrecomp) and the
other snesrecomp game repositories. This repository is ROM-free: you must
supply your own legally obtained copy.

**Just want to build and play it?** See [SETUP.md](SETUP.md) for a
Windows/Linux quick-start. Contributors should read
[CONTRIBUTING.md](CONTRIBUTING.md) instead. For a consolidated map of
everything reverse-engineered about this ROM so far (WRAM variables,
named routines, patch sites, compressed data regions), see
[docs/ROM_MAP.md](docs/ROM_MAP.md).

## Status: Phase 1 (LLE-first correctness baseline)

This bring-up follows snesrecomp's own documented philosophy
(`snesrecomp/docs/LLE_FIRST_ANALYSIS.md`): the interpreter is the
correctness baseline for every game, and AOT-compiled banks are layered on
top only once proven. SimCity currently runs entirely on the shared
interpreter tier (`interp816` over the real PPU/APU/DMA/cart device models),
driven by an accurate H/V master-clock frame loop -- the same technique
snesrecomp's own game-neutral reference driver
(`snesrecomp/cosim/ref_driver.c`) uses, so no SimCity-specific scheduler or
address knowledge was required to reach this milestone.

`bash tools/regen.sh` already runs the real recompiler pipeline against the
ROM from nothing but the architectural vector-table seed in
`recomp/bank00.cfg` (no hand-identified function boundaries) and proves out
152 AOT-eligible / 48 LLE-only functions across 3 banks. Wiring those
generated banks in requires the AOT/`CpuState` hybrid runtime
(`common_cpu_infra.c` / `cpu_state.c` / `interp_bridge.c`) instead of the
standalone `interp816` this phase uses directly -- see "Next phase" below.

### Attract-demo qualification

`build/SimCitySNESRecomp.exe simcity.sfc --qualify N` runs N frames
headless and checks the same generic bar snesrecomp's own per-game status
entries use for a new bring-up (e.g. the SA-1/Super Mario RPG and
DSP-1/Super Mario Kart entries in `snesrecomp/README.md`): logic state
keeps changing, audio stays actively producing non-silent samples, and
rendered video keeps changing -- i.e. "goes through the attract demo
without logic, video, or audio errors," made concrete and re-runnable.

Verified result (3 minutes of simulated game time, 10800 frames):

```
qualify: PASS frames=10800 master=3858886408 logic_changes=10792
audio_samples=5766172 audio_active_frames=10516 video_changes=8676
```

All three axes pass: 10792/10800 frames changed logic state, 10516/10800
frames had active (non-silent) audio, and 8676/10800 frames changed the
rendered framebuffer, with no opcode-guard trip (hang/runaway) at any point.

### Real bugs found and fixed along the way

**Audio permanently froze after ~1 second.** The shared runner's
`dsp_getSamples()` (`snesrecomp/runner/src/snes/dsp.c`) always consumes a
**fixed 534 native samples** per call and resamples them to whatever output
count you ask for -- it does not consume the count you pass it. Gating the
call on "enough samples for my ~533 output request" instead of the real
fixed 534 let it fire when slightly fewer than 534 raw samples existed,
pushing the consumer past the producer; the unsigned
`sampleWrite - sampleRead` wraparound then read as "ring completely full"
to the DSP's own backpressure check and froze sample production forever.
Fixed in `src/main.c` by gating on the real fixed quantum (see the comments
at both `dsp_getSamples` call sites).

**Every button/direction was mapped to the wrong bit.** The shared runner's
auto-joy-read path (`snes.c`) applies a full 16-bit bit-reversal
(`SwapInputBits`) to `input1_currentState` before splitting it into the
`$4218`/`$4219` hardware registers the game reads. The naive "hardware bit
order" mapping (bit15=B, bit8=Right, etc.) is therefore wrong for
`input1_currentState` itself; see the `kPad_*` enum comment in `src/main.c`
for the derivation, verified empirically per-button. Fixed, and confirmed
working end-to-end by interactive testing for all 8 discrete buttons
(A/B/X/Y/L/R/Start/Select).

### D-pad fixed (genuine stock-ROM bug, not a recomp issue)

The D-pad produced zero effect anywhere in the stock ROM -- confirmed dead
on this recomp and then independently re-confirmed on real
hardware-accurate emulation (bsnes), ruling out a recomp-side bug. Root
cause: multiple direction-check sites throughout the ROM read the wrong
half of the 16-bit edge/held-state word -- the low nibble of the byte they
read is hardware-guaranteed zero on any real SNES (unconnected
controller-port pins), so those checks can never fire, on this recomp or on
real hardware. `src/main.c` applies a small, targeted set of ROM data
patches at load time (game-specific, never touching the shared snesrecomp
runtime) that repoint each read at the real edge byte instead. Fixed and
confirmed working by interactive testing across every affected screen: map
scrolling, the build cursor, the toolbar, in-game menus, the Information
panel, the mode-select (start) menu, Scenario Select, Save, Tax, the
Load/Save/Exit menu, the Map Select scenario-number picker, the
city-name-entry on-screen keyboard, the Select-game-level (Easy/Medium/
Hard) screen, the Comprehensive/Information map overlay, and the View
screen (the watch icon). See
[`docs/INVESTIGATION_dpad.md`](docs/INVESTIGATION_dpad.md) for the full
investigation, including the six distinct bug variants found and the live
bsnes tracing that pinned down the last few.

**Known remaining limitations:** the View screen's D-pad now genuinely
updates its underlying game state but nothing visible changes yet --
tracked in the same doc. The map's "fast travel" modifier (holding B or
X while moving, for a faster/bigger scroll jump) is **fixed** -- a 9th,
previously-unpatched site in the same D-pad bug family (see
`docs/INVESTIGATION_dpad.md` "Fast travel: FIXED").

### Graphics/text export tool

`tools/extract_graphics.py` decompresses and exports the game's font
tileset, scenario tileset, and every advisor/scenario dialog text block
(Nintendo's LC_LZ5-style compression, used throughout this ROM) as PNG
and raw `.bin` files, for mod support. Needs Pillow (`pip install
Pillow`); run with `python tools/extract_graphics.py` from the repo
root once your ROM is staged as `simcity.sfc`. Output goes to
`extracted_assets/` (gitignored -- it's derived from the copyrighted
ROM, so it's never committed). Verified against this project's own ROM:
every dialog text block decompresses and renders as readable English
text.

### HDMA execution was entirely missing (also fixed)

Separately from the D-pad bug family: this project's cycle-accurate
execution mode never actually ran HDMA transfers at all (only plain DMA was
wired up), so any HDMA-driven visual effect in this ROM silently did
nothing -- most visibly, the View screen's tilted "photograph on a table"
map, which instead rendered completely flat. See
[`docs/INVESTIGATION_hdma.md`](docs/INVESTIGATION_hdma.md) for the root
cause and fix (implemented game-side in `src/main.c`, no changes to the
shared `snesrecomp` runtime needed).

## Controls (windowed mode)

| SNES button | Key(s) |
|---|---|
| D-pad | Arrow keys, or U/H/J/K (up/left/down/right) |
| A | X |
| B | Y or Z (either physical key position, for QWERTZ keyboards) |
| X | S |
| Y | A |
| L | Q |
| R | E |
| Start | Enter |
| Select | B |
| Fast-forward (hold) | Tab |
| Debug-menu code entry (controller 2, one-shot) | F2 |
| Mouse cursor control (toggle) | F3 |
| Dump WRAM snapshot now (`wram_snapshot.bin`) | F4 |
| Cheat: No Disasters (toggle, unconfirmed bit) | F5 |
| Cheat: Needless Money (toggle, confirmed) | F6 |
| Cheat: Valve Max (toggle, confirmed) | F7 |
| Cheat: Water Reclaim (toggle, unconfirmed bit) | F8 |
| Save state to slot 1-9/0 | Shift+1 .. Shift+9, Shift+0 |
| Load state from slot 1-9/0 | 1 .. 9, 0 |

Save states (`savestate_<digit>.bin`, gitignored) capture the full emulator
state -- WRAM, CPU registers, and every device model -- so a specific
scenario (on the map screen, cursor visible, a particular button held) can
be set up once by hand and then reloaded instantly and deterministically
for repeated testing, instead of re-navigating menus or guessing `--input`
timing on every run. `--load-state <path>` loads one headlessly at startup
(before `--qualify` or the windowed loop begins), so a fixed `--input`
sequence can be replayed against an already-positioned scenario.

A/R advance the title screen itself. To reach the mode-select menu (rather
than auto-continuing into gameplay), press Start or A roughly 10 seconds
after the title screen appears, then wait about 5 more seconds -- see
`docs/INVESTIGATION_dpad.md`.

F2 and F3 aren't SNES buttons -- host-only additions this recomp's C driver
can offer since it isn't limited to 8 controller inputs:

- **F2** queues the documented debug-menu entry code (Left, A, Right, Y,
  Up, B, Down, X, Select, Start, Start, Select, R, R, L, L) on controller
  2 in one shot, timed automatically, instead of 16 hand-timed presses.
  Press it once while on the "Goodbye! See you soon" quit-confirmation
  screen (Load/Save/Exit menu -> END). Unverified against this ROM dump --
  a whole-ROM search found no code reading controller 2 at all, so this
  may not exist in this revision; F2 exists to test it either way. Also
  available headlessly via `SC_DEBUG_CODE_AT=<frame>`.
- **F3** toggles host-mouse control of the game's cursor, ported from the
  community mouse patch (https://github.com/Selicre/simcity-mouse) --
  drives the same WRAM bytes ($7e01eb/$7e01ed) that patch identified,
  directly from real mouse movement instead of emulating an SNES mouse
  peripheral. Off by default. Carries the same caveats upstream documents:
  jank in menus, no button support, occasional resets to origin.

Map/scenario loading ("Please wait...") does genuine procedural
generation work rather than an artificial delay, so it isn't
patched out -- hold fast-forward (Tab) while it's on screen to blow
through it in a couple seconds instead.

## Next phase: AOT/CpuState hybrid tier

To actually run the AOT-compiled banks `tools/regen.sh` already produces
(rather than 100% interpretation), the host needs to move from the
standalone `interp816` driver to the shared `CpuState`/`common_cpu_infra.c`
runtime and understand how SimCity's own main-loop idiom yields control
back to the host once per frame (`snesrecomp/docs/LLE_SCHEDULER.md`
describes the general "auto-quiescent" interpreter mechanism every new
game is meant to use for this, in preference to Mega Man X's older
per-game cooperative-scheduler/fiber approach). That is genuine
per-game bring-up work -- identifying SimCity's own vblank-wait idiom well
enough to declare it safely -- and is intentionally out of scope for this
milestone.

## Building

Prerequisites: a `snesrecomp` checkout (pinned submodule, see
CONTRIBUTING.md), Python 3.9+, Rust (for the native analyzer), CMake +
Ninja or Visual Studio, and SDL2 (e.g. via
[vcpkg](https://github.com/microsoft/vcpkg): `vcpkg install sdl2:x64-windows`).

```bash
git clone --recurse-submodules <this repo>
cd simcity
bash tools/bootstrap.sh
# stage your own legally obtained ROM as simcity.sfc, then:
bash tools/regen.sh --no-tests
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --target SimCitySNESRecomp
./build/SimCitySNESRecomp.exe simcity.sfc                 # windowed
./build/SimCitySNESRecomp.exe simcity.sfc --qualify 3600  # headless qualification
```

See CONTRIBUTING.md for the full checkout/build/PR workflow and how this
repository's framework dependency is managed, and
`docs/INVESTIGATION_dpad.md` for the debugging tools built along the way
(env-gated tracing, PC-reachability bitmap diffing, synthetic `--input`
injection, frame dumps) if you're picking up the View screen issue or a
similar input bug.
