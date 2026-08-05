# SimCitySNESRecomp

Static recompilation of *SimCity* (SNES) onto [snesrecomp](https://github.com/mstan/snesrecomp),
the same general-purpose 65816-to-C framework used by
[MegaManXSNESRecomp](https://github.com/mstan/megamanxsnesrecomp) and the
other snesrecomp game repositories. This repository is ROM-free: you must
supply your own legally obtained copy.

**Just want to build and play it?** See [SETUP.md](SETUP.md) for a
Windows/Linux quick-start. Contributors should read
[CONTRIBUTING.md](CONTRIBUTING.md) instead.

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

**Known remaining limitations:** the "fast travel" modifier (holding X or Y
while moving on the map, for a faster/bigger scroll jump) is not yet fixed;
the View screen's D-pad now genuinely updates its underlying game state but
nothing visible changes yet. Both tracked in the same doc.

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
| Select | Shift |
| Fast-forward (hold) | Tab |

A/R advance the title screen itself. To reach the mode-select menu (rather
than auto-continuing into gameplay), press Start or A roughly 10 seconds
after the title screen appears, then wait about 5 more seconds -- see
`docs/INVESTIGATION_dpad.md`.

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
injection, frame dumps) if you're picking up the fast-travel issue or a
similar input bug.
