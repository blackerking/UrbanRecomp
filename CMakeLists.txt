cmake_minimum_required(VERSION 3.16)
project(SimCitySNESRecomp C)

set(CMAKE_C_STANDARD 11)

# Prerequisites: a snesrecomp checkout at ./snesrecomp (pinned submodule); the
# generated C for the variant you build (run `bash tools/regen.sh` with the
# verified ROM staged, see CONTRIBUTING.md); SDL3 (vcpkg: `sdl3:x64-windows`),
# or SDL2 with -DSNESRECOMP_SDL_BACKEND=SDL2.
set(SNESRECOMP_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp" CACHE PATH
    "Path to the snesrecomp checkout used by this game")

include(${SNESRECOMP_ROOT}/runner/runner.cmake)

# Phase-1 bring-up (see README.md and src/main.c): SimCity runs entirely on
# the shared LLE interpreter tier (standalone interp816 over the device
# models), NOT the AOT/CpuState hybrid runtime. `bash tools/regen.sh` already
# produces real AOT-eligible banks under src/gen/ -- proving the recompiler
# pipeline works end to end against the ROM -- but that generated C targets
# the CpuState ABI (cpu_state.c/common_cpu_infra.c/interp_bridge.c), which
# this executable does not link yet. Wiring that hybrid tier in is the
# documented next phase (see README.md); until then src/gen/*.c is
# intentionally left out of this target so the build doesn't silently
# mismatch ABIs.
if(EXISTS ${CMAKE_SOURCE_DIR}/src/gen)
    file(GLOB SIMCITY_GEN_PROBE ${CMAKE_SOURCE_DIR}/src/gen/*.c)
    if(SIMCITY_GEN_PROBE)
        message(STATUS
            "SimCitySNESRecomp: src/gen has ${SIMCITY_GEN_PROBE} generated "
            "AOT bank(s) from tools/regen.sh, not yet linked -- Phase 1 is "
            "interpreter-only. See CMakeLists.txt / README.md.")
    endif()
endif()

add_executable(SimCitySNESRecomp
    src/main.c
)

target_include_directories(SimCitySNESRecomp PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/recomp
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
)

# Phase-1 needs only the SNES hardware/device models + the standalone
# interp816 core, not the AOT/CpuState runtime (common_rtl.c,
# common_cpu_infra.c, cpu_state.c, interp_bridge.c, debug_server.c) --
# see src/main.c's header comment for why. Compile those sources directly
# rather than the full SNESRECOMP_RUNNER_SOURCES list until the AOT hybrid
# tier is wired.
set(SIMCITY_DEVICE_SOURCES
    ${SNESRECOMP_ROOT}/runner/src/audio_trace.c
    ${SNESRECOMP_ROOT}/runner/src/crc32.c
    ${SNESRECOMP_ROOT}/runner/src/sha256.c
    ${SNESRECOMP_ROOT}/runner/src/util.c
    ${SNESRECOMP_ROOT}/runner/src/snes/apu.c
    ${SNESRECOMP_ROOT}/runner/src/snes/cart.c
    ${SNESRECOMP_ROOT}/runner/src/snes/cx4.c
    ${SNESRECOMP_ROOT}/runner/src/snes/cpu.c
    ${SNESRECOMP_ROOT}/runner/src/snes/dma.c
    ${SNESRECOMP_ROOT}/runner/src/snes/dsp.c
    ${SNESRECOMP_ROOT}/runner/src/snes/dsp1.c
    ${SNESRECOMP_ROOT}/runner/src/snes/dsp1_hle.c
    ${SNESRECOMP_ROOT}/runner/src/snes/joypad.c
    ${SNESRECOMP_ROOT}/runner/src/snes/ppu.c
    ${SNESRECOMP_ROOT}/runner/src/snes/ppu_legacy.c
    ${SNESRECOMP_ROOT}/runner/src/snes/sa1.c
    ${SNESRECOMP_ROOT}/runner/src/snes/snes.c
    ${SNESRECOMP_ROOT}/runner/src/snes/snes_other.c
    ${SNESRECOMP_ROOT}/runner/src/snes/spc.c
    ${SNESRECOMP_ROOT}/runner/src/snes/superfx.c
    ${SNESRECOMP_ROOT}/runner/src/snes/ws_shadow.c
    ${SNESRECOMP_ROOT}/runner/src/snes/interp816.c
    ${SNESRECOMP_ROOT}/runner/src/snes/color_lut.c
)
target_sources(SimCitySNESRecomp PRIVATE ${SIMCITY_DEVICE_SOURCES})


if(NOT MSVC)
    target_compile_options(SimCitySNESRecomp PRIVATE
        -w -Wno-implicit-function-declaration)
endif()

# Desktop SDL comes from the runner helper rather than a per-game
# find_package: it picks the backend (SDL3 by default, SDL2 via
# -DSNESRECOMP_SDL_BACKEND=SDL2), sets SNESRECOMP_SDL3 and SDL_MAIN_HANDLED,
# and copies the runtime DLL beside the exe on Windows. Rolling our own is
# how this project stayed on SDL2 without noticing SDL3 had become the
# default -- see docs/SDL_BACKENDS.md upstream.
# SDL3 is the default (runner.cmake's own default). It was pinned to SDL2 for
# a while after two defects reached a "verified" build through a headless gate
# that could not fail on them: SDL_Init's inverted return convention, and a
# black window from SDL3 defaulting textures to blending while this
# framebuffer carries no alpha. Both fixed, and the window checked by eye.
#
# Build the fallback with -DSNESRECOMP_SDL_BACKEND=SDL2.
#
# Known SDL3 gap: SDL_RenderReadPixels returns black on this backend, so
# write_renderer_ppm() and SC_MENU_PREVIEW do not capture. The framebuffer
# dump (SC_DUMP_AT / write_ppm) is unaffected and is the reliable check --
# it is byte-identical between the two backends.
snesrecomp_target_sdl(SimCitySNESRecomp)

# ── AOT link probe (step 1 of the CpuState migration) ────────────────────────
#
# Optional target, NOT built by default and NOT part of the shipping
# executable. It links the generated banks in src/gen against the shared
# AOT/CpuState runtime and reports the dispatch table -- see src/aot_probe.c
# for why this exists before touching src/main.c.
#
#   cmake --build build --target SimCityAOTProbe
#
# It is excluded from ALL so a broken or stale src/gen can never break the
# normal build, and it is skipped entirely when src/gen has not been generated.
file(GLOB SIMCITY_GEN_SOURCES ${CMAKE_SOURCE_DIR}/src/gen/*.c)
if(SIMCITY_GEN_SOURCES)
    add_executable(SimCityAOTProbe EXCLUDE_FROM_ALL
        src/aot_probe.c
        ${SIMCITY_GEN_SOURCES}
        src/simcity_hle.c
        src/simcity_fiber.c
        src/simcity_fiberdrive.c
        ${SNESRECOMP_ROOT}/runner/src/cpu_state.c
        ${SNESRECOMP_ROOT}/runner/src/common_cpu_infra.c
        ${SNESRECOMP_ROOT}/runner/src/common_rtl.c
        ${SNESRECOMP_ROOT}/runner/src/cpu_trace.c
        ${SNESRECOMP_ROOT}/runner/src/snes/interp_bridge.c
        ${SNESRECOMP_ROOT}/runner/src/snes/tier2_capture.c
        ${SNESRECOMP_ROOT}/runner/src/snes/msu1.c
        ${SNESRECOMP_ROOT}/runner/src/recomp_hw.c
        ${SNESRECOMP_ROOT}/runner/src/ppu_dma_trace.c
        ${SNESRECOMP_ROOT}/runner/src/host_report.c
        ${SNESRECOMP_ROOT}/runner/src/framedump.c
        ${SIMCITY_DEVICE_SOURCES}
    )
    if(MSVC)
        # WORKAROUND, not a solution. cpu_state.c ships an empty fallback
        # g_ram_routine_guards[] marked __attribute__((weak)) -- a GCC/Clang
        # construct MSVC ignores -- so its definition collides with the strong
        # one every generated dispatch_v2.c emits. The portable fix upstream is
        # __declspec(selectany) on MSVC; until then the probe tolerates the
        # duplicate. Do NOT carry this into the shipping target: it would also
        # hide genuine duplicate-symbol bugs.
        target_link_options(SimCityAOTProbe PRIVATE /FORCE:MULTIPLE)
    endif()
    target_include_directories(SimCityAOTProbe PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/recomp
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
        # common_rtl.c includes "config.h", which lives in the desktop runner
        # dir rather than on the shared runner include path.
        ${SNESRECOMP_ROOT}/runner/src/desktop
    )
    if(NOT MSVC)
        target_compile_options(SimCityAOTProbe PRIVATE
            -w -Wno-implicit-function-declaration)
    endif()
    # config.h -> sdl_compat.h -> SDL.h, so the probe needs SDL's headers even
    # though it opens no window.
    snesrecomp_target_sdl(SimCityAOTProbe)
endif()

# -- AOT-tier build of the real host (migration step 2) ----------------------
#
# Same src/main.c as the shipping target, but linked together with the
# generated banks and the AOT/CpuState runtime. Behaviour is identical -- it
# still runs 100% on interp816 -- so this proves only that the two tiers can
# COEXIST in one binary: shared g_ram, no duplicate symbols, no ABI clash.
# Routing execution through the compiled bodies is step 3.
#
#   cmake --build build --target SimCitySNESRecompAOT
if(SIMCITY_GEN_SOURCES)
    add_executable(SimCitySNESRecompAOT EXCLUDE_FROM_ALL
        src/main.c
        ${SIMCITY_GEN_SOURCES}
        src/simcity_hle.c
        src/simcity_fiber.c
        src/simcity_fiberdrive.c
        ${SNESRECOMP_ROOT}/runner/src/cpu_state.c
        ${SNESRECOMP_ROOT}/runner/src/common_cpu_infra.c
        ${SNESRECOMP_ROOT}/runner/src/common_rtl.c
        ${SNESRECOMP_ROOT}/runner/src/cpu_trace.c
        ${SNESRECOMP_ROOT}/runner/src/ppu_dma_trace.c
        ${SNESRECOMP_ROOT}/runner/src/recomp_hw.c
        ${SNESRECOMP_ROOT}/runner/src/host_report.c
        ${SNESRECOMP_ROOT}/runner/src/framedump.c
        ${SNESRECOMP_ROOT}/runner/src/snes/interp_bridge.c
        ${SNESRECOMP_ROOT}/runner/src/snes/tier2_capture.c
        ${SNESRECOMP_ROOT}/runner/src/snes/msu1.c
        ${SIMCITY_DEVICE_SOURCES}
    )
    target_include_directories(SimCitySNESRecompAOT PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/recomp
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
        ${SNESRECOMP_ROOT}/runner/src/desktop
    )
    target_compile_definitions(SimCitySNESRecompAOT PRIVATE
        SIMCITY_AOT_TIER)
    if(MSVC)
        # See the note on SimCityAOTProbe: cpu_state.c's weak-marked fallback
        # guard table is a strong definition under MSVC.
        target_link_options(SimCitySNESRecompAOT PRIVATE /FORCE:MULTIPLE)
    else()
        target_compile_options(SimCitySNESRecompAOT PRIVATE
            -w -Wno-implicit-function-declaration)
    endif()
    snesrecomp_target_sdl(SimCitySNESRecompAOT)
endif()

# -- AOT differential harness (migration step 3a) ----------------------------
#   cmake --build build --target SimCityAOTDiff
if(SIMCITY_GEN_SOURCES)
    add_executable(SimCityAOTDiff EXCLUDE_FROM_ALL
        src/aot_diff.c
        ${SIMCITY_GEN_SOURCES}
        src/simcity_hle.c
        src/simcity_fiber.c
        src/simcity_fiberdrive.c
        ${SNESRECOMP_ROOT}/runner/src/cpu_state.c
        ${SNESRECOMP_ROOT}/runner/src/common_cpu_infra.c
        ${SNESRECOMP_ROOT}/runner/src/common_rtl.c
        ${SNESRECOMP_ROOT}/runner/src/cpu_trace.c
        ${SNESRECOMP_ROOT}/runner/src/ppu_dma_trace.c
        ${SNESRECOMP_ROOT}/runner/src/recomp_hw.c
        ${SNESRECOMP_ROOT}/runner/src/host_report.c
        ${SNESRECOMP_ROOT}/runner/src/framedump.c
        ${SNESRECOMP_ROOT}/runner/src/snes/interp_bridge.c
        ${SNESRECOMP_ROOT}/runner/src/snes/tier2_capture.c
        ${SNESRECOMP_ROOT}/runner/src/snes/msu1.c
        ${SIMCITY_DEVICE_SOURCES}
    )
    target_include_directories(SimCityAOTDiff PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/recomp
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
        ${SNESRECOMP_ROOT}/runner/src/desktop
    )
    if(MSVC)
        target_link_options(SimCityAOTDiff PRIVATE /FORCE:MULTIPLE)
    else()
        target_compile_options(SimCityAOTDiff PRIVATE
            -w -Wno-implicit-function-declaration)
    endif()
    snesrecomp_target_sdl(SimCityAOTDiff)
endif()

# -- Fiber layer self-test ---------------------------------------------------
# Standalone: no ROM, no runtime, no generated code. Coroutine bugs are far
# easier to find here than underneath a running 65816 interpreter.
#   cmake --build build --target SimCityFiberTest && ./build/Release/SimCityFiberTest
add_executable(SimCityFiberTest EXCLUDE_FROM_ALL
    tests/fiber_test.c
    src/simcity_fiber.c
)
target_include_directories(SimCityFiberTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
