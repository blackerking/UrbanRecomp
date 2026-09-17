/*
 * Host implementations of the game routines declared `hle_func` in recomp/.
 *
 * Shared by every AOT-linked target (UrbanRecompAOT, UrbanRecompAOTProbe,
 * UrbanRecompAOTDiff) so there is exactly one copy.
 */
#include <stdio.h>
#include <stdlib.h>

#include "cpu_state.h"
#include "common_rtl.h"
#include "sc_mapgen.h"

/* Set by the host once it is actually driving frames. Until then the yield has
 * nowhere to go -- see the long note in ScHle_WaitForVblank. */
void (*g_sc_yield_to_host)(void) = NULL;

unsigned long g_sc_vblank_hle_calls = 0;

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
 * 3. $c7 MUST KEEP ADVANCING, and this one is specific to this game. $c7 counts
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
RecompReturn ScHle_WaitForVblank(CpuState *cpu) {
    g_sc_vblank_hle_calls++;

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

    if (g_sc_yield_to_host) {
        g_sc_yield_to_host();
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
                    "[hle] ScHle_WaitForVblank called with no host frame "
                    "driver installed -- see docs/MIGRATION_step3.md. "
                    "Returning immediately; frame pacing is NOT happening.\n");
        }
    }

    return RECOMP_RETURN_NORMAL;
}

/*
 * 01:f1ed -- the map generator, replaced wholesale by the decompiled one.
 *
 * This is the point of the whole exercise. 03:d840 runs generation as a single
 * synchronous JSL, and the SNES CPU takes about 800 frames of wall clock to
 * grind through it -- roughly thirteen seconds of the player watching a map
 * appear a few cells at a time. src/sc_mapgen.c does the same work
 * natively in well under a frame.
 *
 * Safe to substitute only because it is verified bit-exact, not merely
 * plausible: three maps covering BOTH branches reproduce the guest's map on
 * all 12000 cells, consume the guest's exact draw count, and leave the PRNG in
 * the guest's exact final state. See the long note in sc_mapgen.c.
 *
 * WHAT THIS MUST GET RIGHT BESIDES THE MAP:
 *
 * 1. THE PRNG STATE. 03:d840 does not touch $59/$5b after this returns, but
 *    everything else in the game draws from the same generator, so leaving it
 *    at the wrong value would desynchronise every later random event. We
 *    write back the state our generator ends on, which is the guest's.
 *
 * 2. THE RETURN. f1ed is reached by JSL, so three bytes come off the stack,
 *    not the two that a JSR-reached HLE like ScHle_WaitForVblank pops.
 *
 * 3. NOT THE NMI SHADOW. f1f5 masks $b1 from $b3 on entry and f225 restores it
 *    on exit. Replacing the whole routine skips both, which is correct --
 *    the pair cancels, and we never disabled anything to restore.
 *
 * The caller has already seeded and pre-stepped the PRNG by the time it gets
 * here (03:d84d through the d862 loop), so there is no seeding to redo: the
 * state in $59/$5b IS the starting point.
 */
unsigned long g_sc_mapgen_hle_calls = 0;

RecompReturn ScHle_MapGen(CpuState *cpu) {
    static ScMapGenState gs;
    ScMapGenPrng pr;

    g_sc_mapgen_hle_calls++;

    pr.s0 = (uint16)(g_ram[0x59] | (g_ram[0x5a] << 8));
    pr.s1 = (uint16)(g_ram[0x5b] | (g_ram[0x5c] << 8));
    pr.t  = (uint16)(g_ram[0x5d] | (g_ram[0x5e] << 8));

    sc_mapgen_generate(&pr, &gs);

    /* The map is at $7F0200 -- bank 7F, so 0x10200 into WRAM. This offset was
     * wrong once (7E, not 7F) and cost a whole reference built on the wrong
     * buffer, so it is worth stating rather than assuming. */
    for (unsigned i = 0; i < SC_MAPGEN_CELLS; i++) {
        g_ram[0x10200 + 2 * i]     = (uint8)(gs.map[i] & 0xff);
        g_ram[0x10200 + 2 * i + 1] = (uint8)((gs.map[i] >> 8) & 0xff);
    }

    g_ram[0x59] = (uint8)(pr.s0 & 0xff);  g_ram[0x5a] = (uint8)(pr.s0 >> 8);
    g_ram[0x5b] = (uint8)(pr.s1 & 0xff);  g_ram[0x5c] = (uint8)(pr.s1 >> 8);
    g_ram[0x5d] = (uint8)(pr.t  & 0xff);  g_ram[0x5e] = (uint8)(pr.t  >> 8);

    if (getenv("SC_MAPGEN_HLE_DIAG")) {
        unsigned nz = 0;
        for (unsigned i = 0; i < SC_MAPGEN_CELLS; i++)
            if (gs.map[i] & 0x3ff) nz++;
        fprintf(stderr, "[mapgen_hle] call %lu: %u cells, %lu draws, "
                        "prng %04X/%04X\n",
                g_sc_mapgen_hle_calls, nz, g_sc_mapgen_prng_steps,
                (unsigned)pr.s0, (unsigned)pr.s1);
    }

    cpu->S = (uint16)(cpu->S + 3);        /* JSL: three bytes, not two */
    return RECOMP_RETURN_NORMAL;
}
