# Adaptive Widescreen mod

The built-in **Adaptive Widescreen** mod uses the shared `recomp-ui` Mods
provider, like Super Metroid's custom renderer. Start the executable without
arguments (or with `--mods`), select **Mods**, enable **Adaptive Widescreen**,
choose a view size, and press **Play**. Settings persist in `sc-video.ini`.
The mod starts disabled in a fresh directory.

See the [screenshot gallery](screenshots/adaptive-renderer/README.md) for the
Mods controls, adaptive landscape/portrait views, and fixed 21:9 gameplay.

The entire original 256x224 view stays visible in every mode. Terrain and
buildings extend into the extra space; the original controls, text, cursor,
and simulation retain their native coordinates. The original view can be
centered or placed at the top left. The window is resizable; **F11** toggles
fullscreen. **F10** still opens the game's existing settings menu.

Draft follow-up: advisor/tutorial/budget dialogs currently follow the original
view's anchor too. In top-left mode, those panels should eventually remain
centered independently of the gameplay/HUD placement. This branch retains the
current behavior while that layout work is deferred.

| View size | Behavior |
| --- | --- |
| Fit to window | Add columns in a wide window or rows in a tall window |
| Fit height | Keep 224 rows; add columns when the window is wide enough |
| Fit width | Keep 256 columns; add rows when the window is tall enough |
| 4:3 | Original view, corrected pixel aspect |
| 8:7 | Original view, square pixels |
| 16:10 | 308x224 canvas |
| 16:9 | 342x224 canvas |
| 21:9 | 448x224 canvas |
| 32:9 | 684x224 canvas |

Fit height/width add bars where filling the other axis would crop the original
view. Fixed presets also use bars when the window has a different aspect.
Logical pixels use a 7:6 pixel aspect for 4:3 SNES presentation, except 8:7.
Canvas dimensions round outward to even pixels, so some ratios have a tiny
rounding border. Each axis caps at 2048 logical pixels; extreme windows use
bars after reaching that cap. Scaling preserves proportions.

## Build and run

Initialize both shared dependencies, including their nested submodules:

```sh
git submodule update --init --recursive
```

On Windows with CMake, MinGW-w64, Ninja and SDL3 installed under
`C:/msys64/mingw64`, run from PowerShell:

```powershell
.\tools\run_custom_renderer.ps1 -Rom 'C:\Roms\SimCity (USA).sfc'
```

`-BuildOnly` builds without opening a window; `-SkipBuild` starts an existing
build. `-CMake` and `-Toolchain` override the executable and MinGW paths.
The script runs from this checkout so its settings, saves and optional Sylt
assets stay local to it. On other platforms, use CMake's native generator:

```sh
cmake -S . -B build-custom -DCMAKE_BUILD_TYPE=Release
cmake --build build-custom --parallel
ctest --test-dir build-custom --output-on-failure
./build-custom/SimCitySNESRecomp /path/to/simcity.sfc --mods
```

No code generation is needed for the default executable. See
[the dependency audit](DEPENDENCY_AUDIT.md) before using the experimental AOT
targets with the updated engine.

Direct launch examples (CLI overrides last for that launch; Play in the Mods
launcher persists settings):

```sh
SimCitySNESRecomp simcity.sfc --widescreen --aspect Fit --window-size 1280x720
SimCitySNESRecomp simcity.sfc --widescreen --aspect Width --window-size 720x1280
SimCitySNESRecomp simcity.sfc --widescreen --aspect 32:9 --fullscreen
SimCitySNESRecomp simcity.sfc --no-widescreen
```

`--video-config FILE` selects an isolated settings file. Supported CLI aspect
names are `Fit`, `Height`, `Width`, `4:3`, `8:7`, `16:10`, `16:9`, `21:9`, `32:9`.

## Renderer scope

The city reconstruction is for the verified unheadered USA ROM, SHA-256
`e9c0bc05511e05a0d7c3e7cc42e761e1e8e532d46f59b9854b6902e1a2e9dd0a`.
It reads the 120x100 live map at WRAM offset `0x10200`, ROM tile tables at
`0x156a9` and `0x14f2d`, and current PPU CHR, palette and brightness at each
scanline. Overlay tiles include their -1,-1 overlap across canvas edges.
World bounds show the backdrop. Taller views use the first/last scanline's
PPU state beyond the hardware's vertical range.

Title scenery, main-menu and scenario-selector wood, fax desk and advisor backdrop also extend.
Other menus retain their native contents over repeated background decoration
or the backdrop. Menu content is not spread across an ultrawide display.
The extra city area is scenery: interactions and game-emitted sprites stay
within the original game's view. Offscreen vehicles/disaster actors are not
reconstructed from simulation objects. Pan normally to interact with a place
first seen in a margin. Other ROM regions and all disaster scenes have not
been qualified for custom rendering.

The renderer never writes WRAM, VRAM, OAM or CGRAM. Each native scanline is
copied byte-for-byte after drawing the added scenery. It uses its own dynamic
surface, independently of the shared PPU's horizontal widening limit. While
the mod is enabled it supersedes legacy `SC_WIDESCREEN` / `SC_HOST_MAP` paths.
Existing game fixes still run identically with the mod enabled and disabled.

## Validation

ROM-free CTests cover full-view containment, extreme windows, scaling/input
coordinates, all Mods choices, toggle/persistence, tile flips, transparent
overlap, map bounds, and PPU immutability.

With Python and Pillow, the integration test runs normal inputs through boot,
main menu, scenario selector, populated San Francisco, fax, advisor and a practice city. It checks every frame's CPU,
WRAM and master clock with the mod on/off, and compares the native view
byte-for-byte in periodic captures. It then pans through all presets, wide,
square and portrait Fit modes, plus top-left positioning.

```sh
python tools/test_adaptive_renderer.py --exe build-custom/SimCitySNESRecomp.exe --rom simcity.sfc --artifacts build-custom/integration
```

The test prints a fresh artifact directory containing logs, PPMs, PNG previews
and a local `stock-route/city.state`. Use that state for the Windows resize
test; it owns and closes only its child process/window:

```sh
python tools/test_adaptive_window.py --exe build-custom/SimCitySNESRecomp.exe --rom simcity.sfc --state ARTIFACTS/stock-route/city.state --artifacts build-custom/integration
```

The resize test covers 16:9, 21:9, 32:9, portrait, square and 4:3 window shapes.
`--screenshots` additionally brings its child forward for desktop screenshots;
the default test does not request foreground focus. Live keyboard/mouse input
is ignored in this scripted test so typing elsewhere cannot change the city.
Review the canvas images for new-world correctness in addition to the
automated size and native-view checks. Pixel equality alone does not establish
that newly exposed scenery is correct.

`tools/test_adaptive_launcher.py` accepts the same arguments as the resize
test. It clicks the actual Mods toggle and 32:9 choice, presses Play, then
checks the persisted settings and launched canvas.

The new local save format preserves PPU CPU-port latches, the host clock and
HDMA walker in addition to the shared machine snapshot. The old unversioned
format omitted those latches: loading it shifted later tile uploads and
corrupted water/shore graphics even with the mod disabled. Old states are
rejected; create new ones with this build. The regression compares a 300-frame
restore against uninterrupted play, including captured pixels. In-game SRAM
city saves are separate from these local emulator snapshots.
