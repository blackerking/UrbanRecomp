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

### D-pad: there was never a stock-ROM bug (runner defect, now fixed)

Earlier revisions of this README claimed the stock ROM shipped a
"D-pad bug family" affecting a dozen screens, fixed by ROM patches. **That
was wrong.** The ROM was correct; the runner was not.

`snesrecomp`'s `snes.c` returned the two halves of the auto-joypad read
transposed. Per hardware the joypad word is

```
bits 15-8 : B, Y, Select, Start, Up, Down, Left, Right
bits  7-0 : A, X, L, R, 0, 0, 0, 0     (low nibble = controller ID)
```

so `$4218` carries A/X/L/R plus four always-zero bits, and `$4219` carries
the D-pad. Returning them the wrong way round put the D-pad in `$011b` and
left `$011c` -- the byte the game actually tests -- reading zero.
`LDA $011b (16-bit) / AND #$0f00` is simply *how you read the D-pad*: it
tests `$011c` bits 0-3. It only looked like a per-site bug because the
emulator was feeding it the wrong byte.

Eleven compensating ROM patches were written against that single defect.
With the runner corrected the two compensations cancel -- applying both
kills input again -- so the patches have been removed. Measured on the
bank-loan dialog (its handler depends on exactly one patched site), holding
Right: patches applied leaves the selection stuck, patches removed moves it,
on stock ROM code. Holding Left now yields `$011b=00 / $011c=02`, Left in
`$011c` bit 1, exactly as the hardware layout specifies.

The fix lives in the `snesrecomp` submodule and affects every game built on
that runner. It surfaced because a sibling project (Metal Marines) hit the
identical pattern and refused to accept that two unrelated commercial games
shipped the same input bug -- which was the tell here too, and was missed
for a long time. Eleven independent sites appearing to share one bug is an
anomaly to explain, not corroboration to lean on.

[`docs/INVESTIGATION_dpad.md`](docs/INVESTIGATION_dpad.md) is kept as the
trail of that mistake; note it still argues the old model in most places
and is being re-derived.

Two ROM patches remain, both unrelated to input and each standing on its own
evidence: the `$01f3` cursor step-delay tweak and the `00:c0fb` `$7e21b5`
stomp fix.

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

### Scenario map export tool

`tools/extract_maps.py` decodes every scenario map out of the ROM to
`extracted_assets/maps/` — a raw 24000-byte `.bin` in exactly the layout
the game keeps live at `$7F0200` (120×100 cells, one little-endian 16-bit
tile index each), plus a false-colour `.png` preview. No dependencies; run
`python tools/extract_maps.py` from the repo root with your ROM staged as
`simcity.sfc`.

The map format used to be the big open question here — the data is
compressed and two static guesses had already been tried and rejected. It
turned out to be four stages: the same LC_LZ5 compression used everywhere
else in this ROM, then a word-level LZ pass, a run-length pass, and a
final walk that stamps 3×3 building blocks into a zero-filled grid.
[`docs/REFERENCE_map_format.md`](docs/REFERENCE_map_format.md) documents
all four and how each was verified.

Two things fell out of doing it properly rather than guessing again. The
pointer table holds **nine** maps, not eight — the ninth, the tutorial
map, was being read as padding. And the earlier "definitely not LC_LZ5"
result was simply a bad file offset, not a real property of the data; the
existing decompressor handles those streams fine when pointed at the
right byte.

The last two entries are free play and the tutorial map. Both place zero
buildings, where every real scenario stamps 200+ — the tell that they're
maps you start on rather than cities you inherit. Free play's terrain is
drawn as Mario's face, which is worth knowing before assuming a decode
has gone wrong.

### Post-load power dropout (stock-ROM bug, fixed)

After a load the whole city reads as unpowered for several seconds, and
since the decline logic runs during that window, loading a game actively
costs you population.

Found and fixed by **Truttle1** (<https://www.youtube.com/@Truttle1>),
whose patch is what identified **bit 15 (`$8000`) of each 16-bit map
cell** as the power bit. Measured here to confirm it: loading a scenario
and sampling every 60 frames, powered cells sit at **0 for the first
~400 frames**, then jump to 2888 and settle at 3043 — a ~6.7-second
window with nothing powered.

`FIX POWER ON LOAD` in the F10 menu (**on by default**) marks every cell
powered once the map is in place at `03:c8dd`, letting the game's own
power scan clear whatever is genuinely unpowered on its next pass.

Implemented host-side in C rather than by porting Truttle1's bytes — this
repo doesn't vendor third-party work, and doing it from C needs no free
ROM space. It also avoids a quirk of that patch: because it replaces
`STZ $003a ; RTS` with a 4-byte `JSL`, its `RTL` lands on `03:c8e1` and
runs the SRAM loader a second time (harmless — an idempotent copy that
doesn't touch `$7F0200`, so the power bits survive).

The trigger point `03:c8dd` is **confirmed**: a bsnes exec breakpoint
there fires on loading a saved game, reached via `00:c845` → `03:c8a0`.
This project's own headless harness still cannot drive that path (the
mode-freeze technique doesn't reach it), so the confirmation is from
bsnes, not from `--qualify`.

**Root cause**, found by a bsnes write breakpoint on `$7F0200`: the
routine that makes bit 15 mean "powered" is `03:b152`, which walks all
12000 cells applying a **packed power bitmap held at `$7FA598`** (one bit
per cell, `AND #$7fff` then conditionally `ORA #$8000`). That bitmap is
*not* part of the SRAM save block — the load path restores `$7F5FC0` and
`$7F6560` but nothing at `$7FA598` — so after a load it has to be
recomputed from scratch, and until it is, every cell reads unpowered.
Hence the dropout, and hence why "assume powered until the real scan says
otherwise" is the right shape of fix: `03:b152` corrects it on its next
pass either way.

### HDMA execution was entirely missing (also fixed)

Unrelated to the joypad-register defect above: this project's cycle-accurate
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
| B | Y or Z (either physical key position, for QWERTZ keyboards), or left mouse click |
| X | S |
| Y | A |
| L | Q |
| R | E |
| Start | Enter |
| Select | B |
| Fast-forward (hold) | Tab |
| Debug-menu code entry (controller 2, one-shot) | F2 |
| Mouse cursor control (toggle) | F3 |
| Fast D-pad cursor -- host-speed, bypasses the ROM's own cadence (toggle) | F9 |
| Dump WRAM snapshot now (`wram_snapshot.bin`) | F4 |
| Cheat: No Disasters (toggle, unconfirmed bit) | F5 |
| Cheat: Needless Money (toggle, confirmed) | F6 |
| Cheat: Valve Max (toggle, confirmed) | F7 |
| Cheat: Water Reclaim (toggle, unconfirmed bit) | F8 |
| Save state to slot 1-9/0 | Shift+1 .. Shift+9, Shift+0 |
| Load state from slot 1-9/0 | 1 .. 9, 0 |
| Settings menu (toggle) | F10 |

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
  peripheral. Off by default. Left mouse click doubles as SNES B (see
  "B" in the controls table above) so the mouse alone can point and
  confirm/select. Otherwise carries the same caveats upstream documents:
  jank in menus, occasional resets to origin.
- **F9** toggles a fast D-pad cursor: while a direction is held, pokes
  $01eb/$01ed directly every frame (reusing F3's own `apply_mouse_delta`)
  instead of waiting on the ROM's own cursor cadence, which -- per
  `docs/INVESTIGATION_cursor_cadence.md` -- is genuinely paced by the
  game's own cooperative scheduler (a bank-$03 city-simulation tick
  periodically pre-empting the cursor's turn), not something a simple ROM
  patch can remove. Purely additive: normal D-pad input still reaches the
  game as usual (menus, list navigation, etc. all still work normally),
  this just adds extra host-driven movement on top for the main-map
  cursor specifically. Off by default; same "not authentic ROM timing"
  caveat as F3.
- **F10** opens a settings menu (host-side overlay, not an SNES screen)
  listing this project's own toggles/cheats/save-load actions in one
  generic list -- Up/Down to select, Left/Right/Enter to toggle or
  activate, F10 again to close. The game freezes while it's open (the
  last frame just stays on screen). Pattern researched from ar-recomp
  (ActRaiser recomp)'s settings system: a single table of
  label/type/target-pointer rows drives a generic renderer/input handler
  instead of one hand-coded screen per setting, so adding a new toggle
  later is one line in `src/main.c`'s `s_settings[]`, not a new UI. Covers
  a handful of existing toggles for now (mouse cursor, fast cursor, auto
  turbo, the four debug cheat bits, save/load slot 1, and UNLOCK
  SCENARIOS below); everything else above still needs its own hotkey.

**UNLOCK SCENARIOS** (F10 menu, or `SC_UNLOCK_ALL=1` headless) sets the
scenario completion "win marks" the game keeps in cartridge SRAM at
`$700007` — bits 0-6, i.e. the six ordinary scenarios plus the hidden Las
Vegas — leaving free play and the tutorial alone, since neither is a
scenario and neither has anything to win. Setting all six also sets the
game's own "every scenario beaten" bit, exactly as `03:e31c` does.

It writes only what the ROM's own commit path writes: the flag word, the
header checksum at `$70000e`, and the mirrored backup copy at `$707ff0`.
Measured on a real save: exactly 8 bytes change, the 4 header bytes and
their 4 copies. Skipping the checksum or the backup would get the edit
silently reverted at the next verify (`03:e411`/`03:e446`), which is the
trap here.

Confirmed working by executing it, not just by reading the ROM: with the
completion bit clear the select screen computes a maximum column of 2,
and with bit 15 set it computes 3 — the fourth column, where Las Vegas
and free play live. See `docs/ROM_MAP.md` ("Screen-mode dispatch") for
the `SC_FREEZE=14:<mode>` technique that makes any screen testable
without a save state positioned on it.

Why setting that one bit is enough: the select screen is a 4-column ×
2-row grid, but `03:ddba` computes the maximum reachable column as
`2 + (bit 15 of the completion field)`. The six ordinary scenarios sit in
columns 0-2; Las Vegas and free play sit alone in column 3, at x=`$0fe`,
past the right edge of a 256-wide screen. So beating all six is literally
what widens the grid by one column and lets you scroll right to them —
matching how the screen behaves in play. The full chain is
`$700007` bit 15 → `$42` (`03:e36c`) → `$79` (`03:ddbe`) → the
right-scroll clamp (`03:dde4`) → `$40` = 6 or 7 (`03:de1a`) → the map
pointer table.

It deliberately does nothing until the game has formatted SRAM itself
(magic `"SIM"` present) rather than fabricating a header — so it takes
effect in a real session or from an in-game save state, not from a cold
boot into the attract demo, where the SRAM subsystem never runs at all.
SRAM isn't persisted to disk by this host, so the unlock lasts for the
session and is captured by save states. `SC_SRAM_DUMP_PATH=<file>` dumps
the 32KB SRAM window with a decoded header line to check it took —
ordinary WRAM dumps can't, since SRAM lives in the cart model, not
`g_ram`.

Map/scenario loading ("Please wait...") does genuine procedural
generation work rather than an artificial delay, so it isn't
patched out -- hold fast-forward (Tab) while it's on screen to blow
through it in a couple seconds instead.

There used to be an *automatic* fast-forward that detected the load
screen and applied the same speed-up without Tab. It's now **off by
default** ("AUTO TURBO" in the F10 menu re-enables it), because one of
its three trigger addresses (`00:824b`) turned out to be the game's
**PRNG** — called constantly throughout ordinary simulation — rather
than map-generation-specific code (earlier revisions of this README
called it a "shared checksum/hash routine"; it takes no input and only
advances `$59`/`$5b`, so it was never a checksum),
so it also fired during ordinary play -- each hit arming a 20-frame 6x
burst. Measured with `SC_ADDR_TRACE` against real gameplay save states:
sporadic on the classic map screen (~4 hits per 2000 frames), but
roughly every 10-13 frames on the View screen, i.e. that screen sat in
effectively continuous turbo. That made the game feel rough and badly
worsened the known fast-forward audio-delay problem. `00:824b` has also
been dropped from the trigger set entirely (it was redundant anyway --
`03:d862`, the map-gen loop that *calls* it, is itself a trigger), so
the feature behaves sanely if switched back on.

## AOT frontier: where the static coverage actually stops

`tools/regen.sh` reports the analyzer's static frontier, and it is worth
tracking as the real progress metric for this project — the interpreter
runs everything today, so this number is what "static recompilation"
means concretely.

| | roots | exact variants | AOT-eligible | LLE-only |
|---|---|---|---|---|
| architectural vectors only | 26 | 314 | — | — |
| \+ screen-mode + map-path entries | 60 | 383 | 254 | 129 |
| \+ COP service entries | 71 | **411** | **275** (11,421 insns) | 136 (4,805 insns) |

The two things that moved it were both **indirect dispatch tables read
straight out of the ROM**, which is exactly what a static closure cannot
follow: the 23-entry screen-handler table behind `03:d289`'s
`JMP ($d255,X)`, and the 11-entry COP service table behind `00:8211`'s
`JSR ($8223,X)`. Neither was guessed; both were decoded and cross-checked
(COP service 8 is the LC_LZ5 decompressor, matching every
`LDA #$0008 ; COP #$00` call site in the ROM).

### The blocker: `COP` is this game's syscall instruction

This ROM uses `COP #$00` as a general syscall — **309 call sites**, with a
service number in `A` selecting one of 11 handlers. The analyzer treats
`COP` as a decode terminator (`insn.rs`: `if insn.mnem == "BRK" || insn.mnem
== "COP" { return false }`), so any function containing one is truncated
there and cannot be proven AOT-eligible.

Measured on the current manifest:

- `cop_at_*` is the **single largest LLE-only reason** (165 mentions, 93
  distinct COP sites named)
- **69 of the 136 LLE-only nodes** are blocked by a COP site
- those account for **3,151 of the 4,805 LLE-only instructions — 66%**

Declaring the service targets as roots (done above) makes the handlers
themselves reachable, but it cannot help the *callers*: the caller still
has an unprovable edge mid-function. Reaching high static coverage on this
game needs the framework to model `COP #$imm` as a call-with-return
through the COP vector, the way it already models JSL dispatch helpers
with inline tables. That is an upstream `snesrecomp` change, not something
this repo can fix in a cfg.

## Next phase: AOT/CpuState hybrid tier

To actually run the AOT-compiled banks `tools/regen.sh` already produces
(rather than 100% interpretation), the host needs to move from the
standalone `interp816` driver to the shared `CpuState`/`common_cpu_infra.c`
runtime and understand how SimCity's own main-loop idiom yields control
back to the host once per frame (`snesrecomp/docs/LLE_SCHEDULER.md`
describes the general "auto-quiescent" interpreter mechanism every new
game is meant to use for this, in preference to Mega Man X's older
per-game cooperative-scheduler/fiber approach).

### SimCity's vblank-wait idiom: found

That doc says the only per-game knowledge the LLE scheduler tier needs is
*"which PCs are the yield/die primitives"*. For SimCity that is
**`00:930d`, reached as `COP #$00` with `A = 0`** — the most-used service
in the ROM at 133 call sites:

```
00:930d  SEP #$20
00:930f  STZ $b9          ; clear the frame flag
00:9311  INC $c7          ; free-running spin counter
00:9313  LDA $b9
00:9315  BEQ $9311        ; spin until NMI releases it
00:9317  RTS
```

and the NMI handler closes the loop at `00:80bc` with `INC $b9` (gated on
bit 7 of `$00b1`, which `COP` service 4 at `00:8e75` sets). Confirmed by
bsnes trace, not inferred.

That also explains `$c7`: it counts spin iterations spent waiting for
vblank, and `00:823e` seeds the PRNG from it (`LDA $c7` → `$59`/`$5b`/`$5d`)
— timing-derived randomness, which is why the generated map depends on how
long the player took to get there.

This is the seam the hybrid tier needs. It is a plain `RTS`-returning
primitive rather than Mega Man X's coroutine switch, so it should suit the
fiber-free `hle_func` + NLR-unwind pattern that doc describes without the
stack-corruption problem MMX's yield had.

**TODO**: harmonise with [ar-recomp](https://github.com/DerrickGold/ar-recomp)
-- not yet investigated in this repo; worth a look at what conventions or
shared approach it uses before diverging further.

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
