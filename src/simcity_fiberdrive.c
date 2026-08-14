/*
 * Fiber-mode guest driver -- migration step 3d.
 *
 * docs/MIGRATION_step3.md §5 concluded that SimCity could not use ar-recomp's
 * design: "there is no compiled entry point to hand the fiber", because both
 * architectural entry points were `lle_only`.
 *
 *     I_RESET   00:8000  unproven_call_at_008056_to_03D283  (mode dispatcher)
 *     NMI       00:80b2  unproven_call_at_00813A_to_00C3F9
 *
 * **That is no longer true for RESET.** `03:d283` is compiled now, so
 * `008000:M1X1` is `aot_eligible` with an empty `reasons` list and the
 * dispatch table carries it as `I_RESET_M1X1`. The straight transplant is
 * available for the reset path after all, and this file takes it.
 *
 * NMI is still LLE, blocked through `00:C3F9`'s `m0x1` demand -- a variant the
 * machine never enters (docs/OPEN_QUESTIONS.md). It does not need compiling
 * for this model: the host owns the frame boundary and releases the vblank
 * wait itself, which is exactly what the real handler's `INC $b9` at 00:80bc
 * does, and what ar-recomp's host does with `forceNmi`/`nmiAvail`.
 *
 * Separate translation unit on purpose: this needs cpu_state.h, and that
 * header declares a global `CpuState g_cpu` which collides with src/main.c's
 * `Interp816 *g_cpu`. Isolating it is cheaper than renaming a symbol used
 * several hundred times over there.
 */
#include <stdio.h>

#include "cpu_state.h"
#include "common_rtl.h"
#include "simcity_fiber.h"
#include "simcity_fiberdrive.h"

/* Emitted into src/gen/dispatch_v2.c; declared here rather than pulling in
 * the whole generated header. */
extern RecompReturn I_RESET_M1X1(CpuState *cpu);

/* src/simcity_hle.c */
extern void (*g_simcity_yield_to_host)(void);
extern unsigned long g_simcity_vblank_hle_calls;

static CpuState s_cpu;
static bool     s_returned;     /* I_RESET came back -- the ROM never does */
static bool     s_started;

static void fiber_entry(void) {
    cpu_state_init(&s_cpu, g_ram);
    /* 65816 reset contract, after the ROM's own prologue clears emulation:
     * native mode, 8-bit A and index, stack in page 1, DB and D zeroed. */
    s_cpu.PB = 0x00;
    s_cpu.DB = 0x00;
    s_cpu.D  = 0x0000;
    s_cpu.S  = 0x01ff;
    s_cpu.m_flag = 1;
    s_cpu.x_flag = 1;
    s_cpu.emulation = 0;
    s_cpu.P = 0x30;
    cpu_p_to_mirrors(&s_cpu);
    /* The reset handler is the bottom of the guest's stack: there is no host
     * caller for it to return to. */
    s_cpu.host_return_valid = 0;

    fprintf(stderr, "[fiber] entering I_RESET_M1X1\n");
    I_RESET_M1X1(&s_cpu);
    fprintf(stderr, "[fiber] I_RESET_M1X1 RETURNED\n");

    /* Reaching here means the compiled reset handler returned, which the real
     * ROM never does. Record it rather than letting the fiber trampoline park
     * us silently in its yield loop, where it would look like a hang. */
    s_returned = true;
}

bool SimCityFiberDrive_Init(void) {
    if (s_started) return true;
    if (!SimCityFiber_Create(fiber_entry)) return false;
    /* Only now is a yield safe. Installing this earlier would let the HLE
     * yield into a host that is not inside SimCityFiber_RunOneFrame(). */
    g_simcity_yield_to_host = SimCityFiber_YieldToHost;
    s_started = true;
    return true;
}

bool SimCityFiberDrive_RunGuestFrame(uint64_t frame) {
    unsigned long yields_before = g_simcity_fiber_yields;

    SimCityFiber_RunOneFrame();

    if (s_returned) {
        fprintf(stderr, "[fiber] I_RESET_M1X1 returned at frame %llu -- the "
                "reset handler is not supposed to return; stopping.\n",
                (unsigned long long)frame);
        return false;
    }

    /* A frame with no yield means the guest is not reaching the vblank wait.
     * Tolerate a burst of them -- boot legitimately runs a long way before the
     * first wait -- but do not let the host spin forever reporting frames that
     * are not happening. */
    if (g_simcity_fiber_yields == yields_before) {
        static unsigned quiet;
        if (++quiet > 600) {
            fprintf(stderr, "[fiber] no yield for %u frames (vblank hle calls "
                    "= %lu) -- the guest is not reaching the vblank wait.\n",
                    quiet, g_simcity_vblank_hle_calls);
            return false;
        }
    } else {
        static unsigned quiet_reset;
        (void)quiet_reset;
    }
    return true;
}
