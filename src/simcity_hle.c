/*
 * Host implementations of SimCity routines declared `hle_func` in recomp/.
 *
 * Shared by every AOT-linked target (SimCitySNESRecompAOT, SimCityAOTProbe,
 * SimCityAOTDiff) so there is exactly one copy.
 */
#include <stdio.h>
#include <stdlib.h>

#include "cpu_state.h"
#include "common_rtl.h"

/* Set by the host once it is actually driving frames. Until then the yield has
 * nowhere to go -- see the long note in SimCity_WaitForVblank. */
void (*g_simcity_yield_to_host)(void) = NULL;

unsigned long g_simcity_vblank_hle_calls = 0;

/*
 * 00:930d -- COP service 0, the game's wait-for-vblank primitive and the most
 * used service in the ROM (133 call sites). The real routine is:
 *
 *     SEP #$20 ; STZ $b9 ; INC $c7 ; LDA $b9 ; BEQ -6 ; RTS
 *
 * It clears $b9, spins incrementing $c7, and exits when the NMI handler
 * releases it with INC $b9 at 00:80bc. This is the analogue of ar-recomp's
 * `hle_func 8418 ActRaiser_WaitForVblank`, and the point at which the game
 * hands a frame back to the host.
 *
 * Three things have to be right, and two of them are easy to miss.
 *
 * 1. THE RETURN FRAME. This HLE replaces the whole ROM routine *including its
 *    RTS*, so it must pop the caller's 2-byte JSR frame itself. Skip that and
 *    S leaks 2 bytes per call -- at 133 call sites and one call per frame it
 *    marches the stack down out of page 1 and into zero page, corrupting game
 *    variables. ar-recomp hit exactly this and documents it at
 *    ActRaiser_WaitForVblank.
 *
 * 2. $b9 MUST END NON-ZERO. That is the routine's exit condition, so callers
 *    that re-read it see a released flag, not a stuck one.
 *
 * 3. $c7 MUST KEEP ADVANCING, and this one is specific to SimCity. $c7 counts
 *    spin iterations, and 00:823e seeds the PRNG from it ($c7 -> $59/$5b/$5d),
 *    which is where the map generator's entropy comes from -- see
 *    docs/ROM_MAP.md. An HLE that skips the spin freezes $c7, and every
 *    "random" map the game generates would come out identical. So the count is
 *    advanced from the master clock, preserving the timing-derived character
 *    of the original even though the exact iteration count is not reproduced.
 *
 * Anything that assumed a routine like this is a pure no-op would produce a
 * game that runs and looks fine while silently generating one map forever.
 */
RecompReturn SimCity_WaitForVblank(CpuState *cpu) {
    g_simcity_vblank_hle_calls++;

    cpu->S = (uint16)(cpu->S + 2);                    /* (1) */

    /* (3) -- advance $c7 by a clock-derived amount before releasing. The real
     * spin is ~6 cycles an iteration; the exact figure does not matter, only
     * that it varies with how long the wait actually was. */
    {
        static uint64 last_master = 0;
        uint64 now = cpu->master_cycles;
        uint64 delta = now > last_master ? now - last_master : 0;
        last_master = now;
        g_ram[0xc7] = (uint8)(g_ram[0xc7] + (uint8)((delta / 6u) & 0xffu));
    }

    g_ram[0xb9] = 1;                                  /* (2) */

    if (g_simcity_yield_to_host) {
        g_simcity_yield_to_host();
    } else {
        /* No frame driver yet: src/main.c still runs every instruction on
         * interp816, so nothing calls this and the branch is unreachable in
         * practice. If it ever does fire before the host is driving frames,
         * say so loudly rather than silently returning a frame that never
         * happened -- the game would run with no vblank pacing at all. */
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr,
                    "[hle] SimCity_WaitForVblank called with no host frame "
                    "driver installed -- see docs/MIGRATION_step3.md. "
                    "Returning immediately; frame pacing is NOT happening.\n");
        }
    }

    return RECOMP_RETURN_NORMAL;
}
