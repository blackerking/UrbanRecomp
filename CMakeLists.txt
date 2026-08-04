cmake_minimum_required(VERSION 3.16)
project(SimCitySNESRecomp C)

set(CMAKE_C_STANDARD 11)

# Prerequisites: a snesrecomp checkout at ./snesrecomp (pinned submodule); the
# generated C for the variant you build (run `bash tools/regen.sh` with the
# verified ROM staged, see CONTRIBUTING.md); SDL2 (vcpkg: `sdl2:x64-windows`,
# or your platform's SDL2 dev package).
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

target_compile_definitions(SimCitySNESRecomp PRIVATE SDL_MAIN_HANDLED)

if(NOT MSVC)
    target_compile_options(SimCitySNESRecomp PRIVATE
        -w -Wno-implicit-function-declaration)
endif()

find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2)
    target_link_libraries(SimCitySNESRecomp PRIVATE SDL2::SDL2)
else()
    find_package(SDL2 REQUIRED)
    target_include_directories(SimCitySNESRecomp PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(SimCitySNESRecomp PRIVATE ${SDL2_LIBRARIES})
endif()

if(WIN32)
    add_custom_command(TARGET SimCitySNESRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2>
            $<TARGET_FILE_DIR:SimCitySNESRecomp>
        COMMENT "Copying SDL2.dll next to SimCitySNESRecomp.exe")
endif()
