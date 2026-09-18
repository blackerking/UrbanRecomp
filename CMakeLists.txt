cmake_minimum_required(VERSION 3.16)
project(UrbanRecomp C)

set(CMAKE_C_STANDARD 11)

# Prerequisites: a snesrecomp checkout at ./snesrecomp (pinned submodule); the
# generated C for the variant you build (run `bash tools/regen.sh` with the
# verified ROM staged, see CONTRIBUTING.md); SDL3 (vcpkg: `sdl3:x64-windows`),
# or SDL2 with -DSNESRECOMP_SDL_BACKEND=SDL2.
set(SNESRECOMP_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp" CACHE PATH
    "Path to the snesrecomp checkout used by this game")

include(${SNESRECOMP_ROOT}/runner/runner.cmake)

# The pre-boot launcher (settings, key bindings, the Sylt mod) comes from the
# shared recomp-ui submodule. Without the checkout the game builds and starts
# directly, with the settings in sc-settings.ini.
set(RECOMP_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui" CACHE PATH
    "Shared launcher (recomp-ui) checkout")
if(EXISTS "${RECOMP_UI_ROOT}/recomp_ui.cmake")
    set(RECOMP_UI_ENABLE_MODS ON CACHE BOOL "Urban Recomp launcher mods (Sylt)" FORCE)
    include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
    set(SC_HAVE_LAUNCHER ON)
else()
    message(STATUS "UrbanRecomp: no recomp-ui checkout, building without "
                   "the launcher (git submodule update --init recomp-ui)")
    set(SC_HAVE_LAUNCHER OFF)
endif()

# Phase-1 bring-up (see docs/AOT_LLE.md and src/main.c): the game runs entirely on
# the shared LLE interpreter tier (standalone interp816 over the device
# models), NOT the AOT/CpuState hybrid runtime. `bash tools/regen.sh` already
# produces real AOT-eligible banks under src/gen/ -- proving the recompiler
# pipeline works end to end against the ROM -- but that generated C targets
# the CpuState ABI (cpu_state.c/common_cpu_infra.c/interp_bridge.c), which
# this executable does not link yet. Wiring that hybrid tier in is the
# documented next phase (see docs/AOT_LLE.md); until then src/gen/*.c is
# intentionally left out of this target so the build doesn't silently
# mismatch ABIs.
if(EXISTS ${CMAKE_SOURCE_DIR}/src/gen)
    file(GLOB SC_GEN_PROBE ${CMAKE_SOURCE_DIR}/src/gen/*.c)
    if(SC_GEN_PROBE)
        message(STATUS
            "UrbanRecomp: src/gen has ${SC_GEN_PROBE} generated "
            "AOT bank(s) from tools/regen.sh, not yet linked -- Phase 1 is "
            "interpreter-only. See CMakeLists.txt / docs/AOT_LLE.md.")
    endif()
endif()

add_executable(UrbanRecomp
    src/main.c
    src/sc_launcher.c
    src/sc_sram.c
    src/sc_mapview.c
    src/sc_mapgen.c
        src/sc_decomp.c
)

target_include_directories(UrbanRecomp PRIVATE
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
set(SC_DEVICE_SOURCES
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
target_sources(UrbanRecomp PRIVATE ${SC_DEVICE_SOURCES})

# A trace build (-DSNESRECOMP_ENABLE_TRACE=ON) turns on the observability
# rings in cpu_trace.h, and those call into the debug server. This target
# hand-picks its sources instead of taking SNESRECOMP_RUNNER_SOURCES whole,
# so runner.cmake's own trace block never reaches it and the link fails on a
# dozen debug_server_* symbols. Add them here, for trace builds only.
if(SNESRECOMP_ENABLE_TRACE)
    target_sources(UrbanRecomp PRIVATE
        ${SNESRECOMP_ROOT}/runner/src/debug_server.c
        ${SNESRECOMP_ROOT}/runner/src/desktop/post_mortem.c)
endif()


if(NOT MSVC)
    target_compile_options(UrbanRecomp PRIVATE
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
snesrecomp_target_sdl(UrbanRecomp)
if(SC_HAVE_LAUNCHER)
    # Box art and window icon are derived from assets/urbanrecomp_logo.png by
    # tools/make_brand_assets.py; src/sc_launcher.c names both paths.
    recomp_target_launcher_ui(UrbanRecomp CONSOLE snes
        BOXART ${CMAKE_SOURCE_DIR}/assets/boxart.jpg BOXART_NAME boxart.jpg)
    add_custom_command(TARGET UrbanRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/assets/icon.png
                $<TARGET_FILE_DIR:UrbanRecomp>/assets/img/icon.png
        VERBATIM)
endif()
# The executable's icon on Windows (Explorer, taskbar, every SDL window).
if(WIN32)
    target_sources(UrbanRecomp PRIVATE src/urbanrecomp.rc)
endif()

# ── AOT link probe (step 1 of the CpuState migration) ────────────────────────
#
# Optional target, NOT built by default and NOT part of the shipping
# executable. It links the generated banks in src/gen against the shared
# AOT/CpuState runtime and reports the dispatch table -- see src/aot_probe.c
# for why this exists before touching src/main.c.
#
#   cmake --build build --target UrbanRecompAOTProbe
#
# It is excluded from ALL so a broken or stale src/gen can never break the
# normal build, and it is skipped entirely when src/gen has not been generated.
file(GLOB SC_GEN_SOURCES ${CMAKE_SOURCE_DIR}/src/gen/*.c)

# SC_AOT=OFF builds without the generated banks even when src/gen exists.
# Release packages use it: src/gen is compiled from the ROM, so a binary that
# links it carries ROM-derived code, and the interpreter -- the default at run
# time anyway -- needs none of it.
option(SC_AOT "Link the AOT tier from src/gen when it is present" ON)
if(NOT SC_AOT)
    set(SC_GEN_SOURCES "")
endif()

# ── AOT is the standard build ───────────────────────────────────────────────
#
# When the generated banks are present, UrbanRecomp IS the AOT build: the
# compiled bodies and the HLEs are linked in, and SC_FIBER=1 runs the guest
# through them. That is what makes the map generator HLE at 01:f1ed run,
# which is the point of having an AOT tier at all -- it turns ~800 frames of
# the SNES CPU grinding through 03:d840 into less than one.
#
# Without SC_FIBER the interpreter runs every opcode -- the default, and the
# correctness baseline every --qualify number rests on.
#
# Without src/gen -- a fresh clone before tools/regen.sh -- this whole block is
# skipped and the target builds interpreter-only exactly as it did before.
if(SC_GEN_SOURCES)
    target_sources(UrbanRecomp PRIVATE
        ${SC_GEN_SOURCES}
        src/sc_hle.c
        src/sc_fiber.c
        src/sc_fiberdrive.c
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
    )
    target_include_directories(UrbanRecomp PRIVATE
        ${SNESRECOMP_ROOT}/runner/src/desktop)
    target_compile_definitions(UrbanRecomp PRIVATE SC_AOT_TIER)
    if(MSVC)
        # cpu_state.c's weak-marked fallback guard table is a strong definition
        # under MSVC, and common_rtl.c redefines a handful of APU helpers that
        # main.c also carries. See the note on UrbanRecompAOTProbe.
        target_link_options(UrbanRecomp PRIVATE /FORCE:MULTIPLE)
    else()
        target_compile_options(UrbanRecomp PRIVATE
            -w -Wno-implicit-function-declaration)
    endif()
endif()

if(SC_GEN_SOURCES)
    add_executable(UrbanRecompAOTProbe EXCLUDE_FROM_ALL
        src/aot_probe.c
        ${SC_GEN_SOURCES}
        src/sc_hle.c
        src/sc_mapgen.c
        src/sc_decomp.c
        src/sc_fiber.c
        # src/sc_fiberdrive.c -- NOT linked here.
        #
        # Nothing in these targets uses the ScFiberDrive_* API; only
        # src/main.c does, and that is not compiled into them. Linking it
        # anyway dragged in sc_advance_until_input_ready(), which lives in
        # main.c, so both targets failed with LNK2019.
        #
        # Stubbing that symbol would be wrong: it drains autoJoyTimer before
        # the guest is handed a frame, and skipping it is exactly how the
        # first fiber attempt deadlocked -- the game spins on $4212 at
        # 00:9280 and nothing advances the beam while the guest holds the
        # CPU. A stub would link and then hang.
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
        ${SC_DEVICE_SOURCES}
    )
    if(MSVC)
        # WORKAROUND, not a solution. cpu_state.c ships an empty fallback
        # g_ram_routine_guards[] marked __attribute__((weak)) -- a GCC/Clang
        # construct MSVC ignores -- so its definition collides with the strong
        # one every generated dispatch_v2.c emits. The portable fix upstream is
        # __declspec(selectany) on MSVC; until then the probe tolerates the
        # duplicate. Do NOT carry this into the shipping target: it would also
        # hide genuine duplicate-symbol bugs.
        target_link_options(UrbanRecompAOTProbe PRIVATE /FORCE:MULTIPLE)
    endif()
    target_include_directories(UrbanRecompAOTProbe PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/recomp
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
        # common_rtl.c includes "config.h", which lives in the desktop runner
        # dir rather than on the shared runner include path.
        ${SNESRECOMP_ROOT}/runner/src/desktop
    )
    if(NOT MSVC)
        target_compile_options(UrbanRecompAOTProbe PRIVATE
            -w -Wno-implicit-function-declaration)
    endif()
    # config.h -> sdl_compat.h -> SDL.h, so the probe needs SDL's headers even
    # though it opens no window.
    snesrecomp_target_sdl(UrbanRecompAOTProbe)
endif()

# -- AOT-tier build of the real host (migration step 2) ----------------------
#
# Same src/main.c as the shipping target, but linked together with the
# generated banks and the AOT/CpuState runtime. Behaviour is identical -- it
# still runs 100% on interp816 -- so this proves only that the two tiers can
# COEXIST in one binary: shared g_ram, no duplicate symbols, no ABI clash.
# Routing execution through the compiled bodies is step 3.
#
#   cmake --build build --target UrbanRecompAOT
# NOTE: this now builds the same content as the default target, which links the
# AOT tier itself. Kept because docs/AOT_LLE.md and tools/gen_align_check.py
# name it, and because having a target that is unambiguously the AOT build is
# useful when the default one is configured without src/gen.
if(SC_GEN_SOURCES)
    add_executable(UrbanRecompAOT EXCLUDE_FROM_ALL
        src/main.c
        src/sc_launcher.c
        src/sc_sram.c
        src/sc_mapview.c
        ${SC_GEN_SOURCES}
        src/sc_hle.c
        src/sc_mapgen.c
        src/sc_decomp.c
        src/sc_fiber.c
        src/sc_fiberdrive.c
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
        ${SC_DEVICE_SOURCES}
    )
    target_include_directories(UrbanRecompAOT PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/recomp
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
        ${SNESRECOMP_ROOT}/runner/src/desktop
    )
    target_compile_definitions(UrbanRecompAOT PRIVATE
        SC_AOT_TIER)
    if(MSVC)
        # See the note on UrbanRecompAOTProbe: cpu_state.c's weak-marked fallback
        # guard table is a strong definition under MSVC.
        target_link_options(UrbanRecompAOT PRIVATE /FORCE:MULTIPLE)
    else()
        target_compile_options(UrbanRecompAOT PRIVATE
            -w -Wno-implicit-function-declaration)
    endif()
    snesrecomp_target_sdl(UrbanRecompAOT)
    if(WIN32)
        target_sources(UrbanRecompAOT PRIVATE src/urbanrecomp.rc)
    endif()
endif()

# -- AOT differential harness (migration step 3a) ----------------------------
#   cmake --build build --target UrbanRecompAOTDiff
if(SC_GEN_SOURCES)
    add_executable(UrbanRecompAOTDiff EXCLUDE_FROM_ALL
        src/aot_diff.c
        ${SC_GEN_SOURCES}
        src/sc_hle.c
        src/sc_mapgen.c
        src/sc_decomp.c
        src/sc_fiber.c
        # src/sc_fiberdrive.c -- NOT linked here.
        #
        # Nothing in these targets uses the ScFiberDrive_* API; only
        # src/main.c does, and that is not compiled into them. Linking it
        # anyway dragged in sc_advance_until_input_ready(), which lives in
        # main.c, so both targets failed with LNK2019.
        #
        # Stubbing that symbol would be wrong: it drains autoJoyTimer before
        # the guest is handed a frame, and skipping it is exactly how the
        # first fiber attempt deadlocked -- the game spins on $4212 at
        # 00:9280 and nothing advances the beam while the guest holds the
        # CPU. A stub would link and then hang.
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
        ${SC_DEVICE_SOURCES}
    )
    target_include_directories(UrbanRecompAOTDiff PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/recomp
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
        ${SNESRECOMP_ROOT}/runner/src/desktop
    )
    if(MSVC)
        target_link_options(UrbanRecompAOTDiff PRIVATE /FORCE:MULTIPLE)
    else()
        target_compile_options(UrbanRecompAOTDiff PRIVATE
            -w -Wno-implicit-function-declaration)
    endif()
    snesrecomp_target_sdl(UrbanRecompAOTDiff)
endif()

# -- Fiber layer self-test ---------------------------------------------------
# Standalone: no ROM, no runtime, no generated code. Coroutine bugs are far
# easier to find here than underneath a running 65816 interpreter.
#   cmake --build build --target UrbanRecompFiberTest && ./build/Release/UrbanRecompFiberTest
add_executable(UrbanRecompFiberTest EXCLUDE_FROM_ALL
    tests/fiber_test.c
    src/sc_fiber.c
)
target_include_directories(UrbanRecompFiberTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
