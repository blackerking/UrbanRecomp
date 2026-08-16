# Plan: SDL3, then the runner's frame model

Two changes, in this order, for the session starting 2026-08-17. The order
matters: the first is trivially revertable and validates the same integration
seam the second depends on.

## Why both, and why now

Three upstream capabilities are already built and this repo gets none of them,
for the same reason each time — it bypassed the seam that delivers them:

| capability | delivered through | bypassed by |
|---|---|---|
| mod frame/APU callbacks, MSU-1 packs, resources | `RtlRunFrame` (`snes_mod_runtime_frame_tick_c`) | our own per-opcode loop |
| widescreen (`runner/src/widescreen.c`, `ws_shadow.c`), netplay | runner features | our own `src/main.c` |
| **SDL3** | `runner/runner.cmake` + `snesrecomp_target_sdl()` | our own `find_package(SDL2 CONFIG)` |

Each bypass was reasonable alone. Together they are a recurring tax, and it is
the tax — not any single feature — that argues for moving onto the shared rail.

`docs/PLAN_widescreen.md` already makes half this argument without noticing:
it correctly says widescreen is "a configuration + game-specific glue task",
which is true *for a host on the runner's model* and not for this one.

---

## Part 1 — SDL3 (tasks 8-10)

Separable from everything else. Do it first.

Upstream `docs/SDL_BACKENDS.md`: "SDL3 is the default desktop backend for CMake
game hosts that include `runner/runner.cmake` and call
`snesrecomp_target_sdl(<target>)`."

1. **Adopt the seam.** Replace `find_package(SDL2 CONFIG QUIET)` /
   `SDL2::SDL2` in `CMakeLists.txt` with `runner/runner.cmake` +
   `snesrecomp_target_sdl(<target>)` for all four targets. Switch
   `src/main.c`'s `#include <SDL.h>` to
   `runner/src/desktop/sdl_compat.h`.
2. **Fix what the shim does not cover.** `src/main.c` uses **89** distinct
   `SDL_*` symbols; `sdl_compat.h` is a **44**-entry shim leaning on
   `SDL_ENABLE_OLD_NAMES`. Most constants and types come across unchanged;
   signature changes do not. Expect work in:
   - **audio** — SDL3 replaced the pull callback with an `SDL_AudioStream` the
     app pushes into. This host owns its DSP drain loop, so this is the real
     one.
   - display bounds (float coords), window/renderer creation flags, and
     bool-vs-int return conventions.

   `runner/src/desktop/mmx23_host_main.inc` handles all of these explicitly at
   the call sites — read it rather than inventing.
3. **Verify.** Build both backends into separate directories
   (`-DSNESRECOMP_SDL_BACKEND=SDL2` for the fallback) and hold the usual bar:
   `--qualify 600` PASS, eleven save states byte-identical between tiers, 81
   framework tests, fiber self-test. **If SDL3 and SDL2 disagree on any of it,
   stop and diff the two** — do not carry an unexplained difference into part 2.

---

## Part 2 — the frame model (tasks 11-13)

### What actually blocks B1

Not the entry point, not the fiber, not AOT coverage. From
`docs/MIGRATION_step3.md` §8:

> This host owns a per-opcode device model that no framework driver drives.
> Any design that runs the guest outside `src/main.c`'s opcode loop freezes
> the PPU, and the ROM's boot spins on a beam-derived register before it ever
> reaches a vblank wait.

Concretely: `00:9280 LDA $4212 ; AND #$01 ; BNE $9280` waits on
auto-joypad-busy, which only clears as the beam advances. Both the fiber and
`interp_bridge_run_loop` deadlock there, because neither advances the PPU —
that is `handle_pos_stuff()` in `src/main.c`, and nothing outside that loop
calls it. `interp816_opcode_hook` looks like the seam for this but is **never
called anywhere in the runner**.

### The steps

4. **Stand up a `RtlRunFrame` host beside the existing loop**, env-gated the
   way `SC_FIBER` is. Do not replace `run_one_frame()` in place: the
   per-opcode loop is the differential oracle for the whole migration, and it
   is the only thing that can tell you whether the new host is right. Model on
   `runner/src/desktop/mmx23_host_main.inc`.
5. **Drive the guest.** With the beam advancing, retry
   `src/simcity_fiberdrive.c`. The mapping is already done and correct:
   `interp_bridge_run_loop(cpu, resume, 0x009311, 0x00b9, 0x00)` — `00:930d`'s
   `INC $c7 ; LDA $b9 ; BEQ` spin, flag `$b9`, cleared while waiting.
   Success is concrete and cheap to check: `g_simcity_vblank_hle_calls > 0`
   and `interp_bridge_lle_resume_pc()` advancing frame to frame.
   `SNESRECOMP_YIELD_DIAG=1` is the built-in diagnostic.
6. **Qualify, then decide.** Compare the frame-model host against the
   per-opcode baseline: `logic_changes`, `audio_active_frames`,
   `video_changes`, NMI counts, and WRAM across the eleven save states. Expect
   divergence in cycle-level counters and **none** in logic.

   Only then decide whether the per-opcode loop is retired. It costs the
   dot-level accuracy that found the missing-HDMA bug, so that is a decision
   to take deliberately, not a cleanup to do quietly. Keeping it as a
   verification oracle is a legitimate end state.

### The counter-argument, recorded fairly

`MIGRATION_step3.md` §1 worried option 3 would put the HDMA and audio work at
risk, then retracted it in §4: dropping per-opcode interleaving is not
dropping per-scanline rendering, the host still advances line by line, raster
effects keep working, and ar-recomp ships widescreen and a 3D diorama on
exactly this model. The retraction is sound — but it was written before this
host's per-opcode beam was known to be load-bearing for the *boot path*, so
re-read it with that in mind rather than treating it as settled.

---

## Budget note

Tokens are tight. Part 1 is self-contained and worth doing even if part 2 does
not start: it lands SDL3, proves the `runner.cmake` seam, and changes nothing
about execution. Part 2 without part 1 is possible but wastes the cheap
validation.
