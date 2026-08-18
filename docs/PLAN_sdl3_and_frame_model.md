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

---

## Correction: part 1 was NOT done

Tasks 8-10 were reported complete on the strength of `--qualify`, the
eleven-save-state differential and the test suites. **Every one of those is
headless and never initialises video**, so the gate that cleared the SDL3
migration was structurally incapable of testing it. Two defects got straight
through:

1. `SDL_Init` returns **true** on success in SDL3 where SDL2 returned `0`, so
   `!= 0` read a successful init as failure -- and `SDL_GetError()` was empty,
   because nothing had gone wrong. The window never opened.
2. With that fixed, the window opens and audio plays but **the picture stays
   blank**.

The first is fixed, along with `SDL_RenderReadPixels`, which returns an
`SDL_Surface *` in SDL3 rather than a status. The second is unresolved.

**The default backend is pinned back to SDL2** in `CMakeLists.txt` until the
SDL3 path is verified with a window open. SDL3 is an in-progress branch, not a
completed migration; build it explicitly with `-DSNESRECOMP_SDL_BACKEND=SDL3`.

The lesson generalises past SDL: **a verification bar that cannot fail on the
thing being changed is not verification.** Every number quoted for this
migration was real and none of them touched the renderer.

### Narrowed: it is not the renderer, it is the loop

`SC_SDL_DIAG=1` on the SDL3 build reports the blit as healthy:

```
[sdl] lock=1 pitch=1024 expect=1024 copy=1
```

`SDL_LockTexture` succeeds, the pitch matches `kVideoPitch` exactly, the
`memcpy` runs and `SDL_RenderCopy` succeeds. (`err=Device not found` is a stale
sticky `SDL_GetError()`, not a failure of these calls.) So the texture is
updated and copied correctly and **the rendering path is not the bug.**

The decisive symptom came from watching the window: **the FPS counter in the
title stops advancing.** The main loop is stalling, not mis-drawing. Sound
continues because queued audio drains independently of the loop.

That reframes it entirely, and points at the one item this plan already
predicted would be the hard part:

> **audio** — SDL3 replaced the pull callback with an `SDL_AudioStream` the app
> pushes into. This host owns its DSP drain loop, so this is the real one.

A push into an `SDL_AudioStream` that blocks or waits would stall the frame
loop exactly like this, while previously-queued samples keep playing. That is
the first thing to check: instrument `sc_audio_open` / the per-frame push in
`sc_sdl_compat.h` and see whether the loop is parked inside it.

Worth noting the diagnostic nearly misled: it prints only three times by
design, so "three lines then nothing" looked like a stalled loop and was
actually just the cap. The title bar, not the log, is what identified this.

### Narrowed again: the loop is healthy too

`SC_SDL_DIAG` made periodic (one line per 60 frames) rather than capped at
three:

```
20 lines in a 20s run  ->  a steady 60 fps
```

So the frame loop is **not** stalling either. Combined with the blit
diagnostics, three things are now measured as working in the SDL3 build:

| | status |
|---|---|
| frame loop | iterating at 60 fps |
| `SDL_LockTexture` / pitch / `memcpy` / `SDL_RenderCopy` | all succeed |
| window + renderer creation shim | correct for SDL3 |

Which contradicts the reported symptom (no video, frozen FPS in the title), and
that contradiction is the most useful thing here. Either the presentation step
is dropping the frame after a successful copy, or **the window being watched
was not the instance being measured** — several instances were launched across
this session, including SDL2 and SDL3 builds and short `timeout` runs that open
their own windows.

Rule that out first, before more instrumentation: run exactly one instance,
confirm its PID, and watch that window. It is the cheapest remaining
experiment and it invalidates or confirms every measurement above.

If it survives that, the next suspects in order are `SDL_RenderPresent`
silently failing (its return is currently unchecked), and the title update at
`SDL_SetWindowTitle` — a frozen title with a live loop is itself odd and may be
the clearer signal of the two.

### Fixed: SDL3 defaults textures to blending, and the framebuffer has no alpha

The black screen was alpha. `s_video_pixels` is `ARGB8888` but the PPU never
writes an alpha byte, so every pixel carries `A=0`. Under SDL2 that was
harmless — a texture defaults to `SDL_BLENDMODE_NONE` and alpha is ignored.
**SDL3 defaults the same texture to blending**, so `A=0` renders it fully
transparent.

```c
SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
```

Confirmed by looking at the window: picture.

That explains why every diagnostic said the pipeline was healthy — it *was*.
The loop ran at 60 fps, `SDL_LockTexture` succeeded with the right pitch, the
`memcpy` ran, `SDL_RenderCopy` returned success, `SDL_RenderPresent` returned
success, the output size was right and the renderer belonged to the window.
Every one of those was true and the screen was still black, because the failure
was in how the correct pixels were *composited*, which no return code reports.

### A second bug, still open: the screenshot path

`write_renderer_ppm` still reads back an all-black image (3 non-black bytes of
1,548,288) while the window visibly shows the game. So `SDL_RenderReadPixels`
under SDL3 is not capturing what is displayed, independently of the fix above.
Row-by-row copying with the surface's own pitch — a real bug in the first SDL3
port of that function — did not change it, so the remaining cause is elsewhere.

That matters beyond screenshots: `SC_MENU_PREVIEW` and any future
render-output check depend on it, and while it is broken those checks report
black regardless of truth.

### The method note this whole episode earns

**Four separate times in this thread, my instrumentation was wrong and the
window was right.** The stale-instance theory, the "loop is stalling" reading,
the capped diagnostic, and finally a readback that reports black while the
screen shows a picture. Every measurement I built agreed with itself and
disagreed with reality.

The rule that would have saved all of it: for a change to what is drawn,
**look at it first**, and only then reach for instrumentation — and treat a
verification path you had to modify for the same migration as a suspect, not
as evidence.

### SDL3 is the default again, with one gap

Both defects are fixed and the window has been checked by eye. Re-verified on
SDL3, this time with a real display check in the bar:

| check | result |
|---|---|
| window renders | **confirmed visually** |
| `--qualify 600` | PASS, counters identical to SDL2 |
| five save states, interpreter vs AOT | byte-identical 128 KB WRAM |
| framebuffer dump, SDL2 vs SDL3 | **byte-identical**, 88.5% non-black |

That last row is the one worth keeping as the standing check. It compares the
*framebuffer* through `SC_DUMP_AT` / `write_ppm`, which touches no SDL
rendering API, so it verifies the emulation and the picture content
independently of the backend. It is what should have been in the bar from the
start instead of only headless counters.

**Known gap:** `SDL_RenderReadPixels` returns black on this SDL3 backend, so
`write_renderer_ppm()` and `SC_MENU_PREVIEW` do not capture. A cleaned-up
implementation (convert the returned surface, copy row-by-row with its own
pitch, read before present) did not change it, so the cause is the backend
rather than the call. Overlay capture is therefore SDL2-only for now; the
framebuffer dump covers everything else.
