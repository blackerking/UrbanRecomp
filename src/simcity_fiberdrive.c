/*
 * Fiber-mode guest driver -- migration step 3d.
 *
 * Drives the guest through `interp_bridge_run_loop`: the interpreter runs the
 * real ROM top-level, and every routine it dispatches bounces into a compiled
 * body through the paired ABI. That is the "interpreter-with-bouncing driver"
 * docs/OPEN_QUESTIONS.md B1 asked for, and it is the framework's own model
 * (snesrecomp/docs/LLE_SCHEDULER.md).
 *
 * WHY NOT ar-recomp's PURE COROUTINE -- both reasons, in order of discovery
 *
 * docs/MIGRATION_step3.md §5 ruled it out because "there is no compiled entry
 * point to hand the fiber": both architectural entries were `lle_only`. **That
 * is obsolete for RESET.** `03:d283`, the screen-mode dispatcher whose
 * unproven exit blocked it, is compiled now, so `008000:M1X1` is
 * `aot_eligible` with an empty `reasons` list and the dispatch table carries
 * it as `I_RESET_M1X1`.
 *
 * So the transplant was tried, and it fails for a second, deeper reason.
 * Measured: the fiber switch works and the compiled reset handler is entered,
 * and then the guest never comes back. **Devices only advance while the host
 * holds the fiber**, so any guest loop spinning on a hardware status register
 * deadlocks -- the guest cannot progress, and the host cannot advance the
 * device that would release it. The boot path has exactly one such loop, and
 * it executes in every recorded session:
 *
 *     00:9280  LDA $4212 ; AND #$01 ; BNE $9280     ; auto-joypad busy
 *
 * ar-recomp gets away with a pure coroutine because its waits are all
 * vblank-shaped and HLE'd. SimCity has a hardware-status spin *before* the
 * first vblank wait, so no amount of AOT coverage makes that design boot.
 *
 * Running under the bridge does NOT fix it, and that is the current state of
 * this file. Measured: `interp_bridge_run_loop` never returns from its first
 * call. The bridge advances the APU (`snes_catchupApu`) and accumulates
 * `cpu->master_cycles`, but it never advances the PPU beam -- upstream, the
 * host does that around `RtlRunFrame`, per frame. **This host advances the
 * beam per opcode, from `handle_pos_stuff()` in src/main.c, and nothing
 * outside that loop drives it.** So `$4212` is frozen inside the bridge too,
 * and 00:9280 spins there exactly as it did in the fiber.
 *
 * There is no host hook to fix it with: `interp816_opcode_hook` is declared
 * in interp816.h and defined as a no-op in interp_bridge.c, but is never
 * called anywhere in the runner -- a dead extension point.
 *
 * The bridge is still the right destination, for a reason independent of the
 * beam: it restores the execution bound. `interp_bridge_lle_master_deadline_-
 * reached`, which every generated block already polls, requires
 * `s_lle_sched_depth > 0 && s_interp_bounce_owner_depth > 0` -- both set only
 * by the bridge's scheduler mode. A bare `I_RESET_M1X1()` call has no bound at
 * all, which is why the fiber hang was unbreakable from the host rather than
 * merely slow.
 *
 * WHAT WOULD ACTUALLY FINISH THIS
 *
 * The two device models have to meet. Either the bridge gains a per-opcode
 * host callback (upstream change; the dead `interp816_opcode_hook` is the
 * natural place), or this host adopts the runner's per-frame model and gives
 * up the per-opcode beam advance -- MIGRATION_step3 §1 option 3, which is the
 * larger change and the one the project's accuracy story rests on. Neither is
 * a coverage problem, which is why more AOT work will not move it.
 *
 * Separate translation unit on purpose: this needs cpu_state.h, and that
 * header declares a global `CpuState g_cpu` which collides with src/main.c's
 * `Interp816 *g_cpu`.
 */
#include <stdio.h>

#include "cpu_state.h"
#include "common_rtl.h"
#include "snes/interp_bridge.h"
#include "simcity_fiberdrive.h"

/* src/simcity_hle.c */
extern unsigned long g_simcity_vblank_hle_calls;

/* 00:930d, the vblank wait -- COP service 0, the game's frame boundary:
 *
 *     930d  SEP #$20
 *     930f  STZ $b9
 *     9311  INC $c7        <- loop top, and our yield point
 *     9313  LDA $b9
 *     9315  BEQ $9311
 *     9317  RTS
 *
 * It clears $b9 and spins until the NMI handler's INC $b9 at 00:80bc releases
 * it, so the flag is *cleared while waiting* -- `run_loop`'s explicit-value
 * form, with value 0. (`run_scheduler` assumes MMX's cleared-after-slot-walk
 * shape and would be wrong here.)
 */
#define SC_RESET_PC24   0x008000u
#define SC_YIELD_PC24   0x009311u
#define SC_VBLANK_FLAG  0x00b9u
#define SC_VBLANK_WAITING 0x00u

static CpuState s_cpu;
static bool     s_started;
static uint32_t s_resume_pc24;
static unsigned long s_bails;

bool SimCityFiberDrive_Init(void) {
    if (s_started) return true;

    cpu_state_init(&s_cpu, g_ram);
    /* 65816 reset contract: native mode, 8-bit A and index, stack in page 1,
     * DB and D zeroed. The ROM's own prologue re-establishes most of this;
     * setting it here keeps the first interpreted step well-defined. */
    s_cpu.PB = 0x00;
    s_cpu.DB = 0x00;
    s_cpu.D  = 0x0000;
    s_cpu.S  = 0x01ff;
    s_cpu.m_flag = 1;
    s_cpu.x_flag = 1;
    s_cpu.emulation = 0;
    s_cpu.P = 0x30;
    cpu_p_to_mirrors(&s_cpu);
    s_cpu.host_return_valid = 0;

    s_resume_pc24 = SC_RESET_PC24;
    s_started = true;
    return true;
}

bool SimCityFiberDrive_RunGuestFrame(uint64_t frame) {
    if (!s_started) return false;

    int ok = interp_bridge_run_loop(&s_cpu, s_resume_pc24, SC_YIELD_PC24,
                                    SC_VBLANK_FLAG, SC_VBLANK_WAITING);

    /* Resume where the wait blocked, not at the reset vector: re-entering at
     * 00:8000 every frame would reboot the game once per frame, which looks
     * superficially like it is running. */
    uint32_t resume = (uint32_t)interp_bridge_lle_resume_pc();
    if (resume) s_resume_pc24 = resume;

    if (!ok) {
        /* 0 = the bridge hit its iteration cap. One is not fatal (a long
         * boot step can outrun it), a run of them means no progress. */
        if (++s_bails > 120) {
            fprintf(stderr, "[fiber] bridge bailed %lu times by frame %llu "
                    "(vblank hle calls = %lu, resume = $%06X) -- the guest is "
                    "not reaching the vblank wait.\n",
                    s_bails, (unsigned long long)frame,
                    g_simcity_vblank_hle_calls, (unsigned)s_resume_pc24);
            return false;
        }
    } else {
        s_bails = 0;
    }
    return true;
}
