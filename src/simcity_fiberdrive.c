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
 * Running under the bridge DOES fix it, and this host now works. Three things
 * had to be true at once, and each was found by measurement:
 *
 *   1. `g_dma` and friends must be published. src/main.c builds its Snes with
 *      snes_init() and never calls SnesInit(), so the AOT bus globals stayed
 *      NULL; the first hardware write the bridge routed through WriteReg
 *      ($4300) called dma_write(NULL, ...) and never returned. That was the
 *      frame-2 wedge, and it is not a coverage problem at all.
 *
 *   2. NMI must be DELIVERED, not simulated. See the long note in
 *      RunGuestFrame() below.
 *
 *   3. The APU must be paced off total elapsed master time. See the note in
 *      run_one_frame_fiber() in src/main.c -- the beam is advanced from two
 *      places at once, and pacing the SPC off only one of them starved it.
 *
 * The $4212 spin is no longer a deadlock either: sc_advance_until_input_ready()
 * simply declines to hand over a frame whose input latch is still busy, which
 * costs ~4224 master cycles at the top of the frame and needs no HLE.
 *
 * WHAT IS AND IS NOT ESTABLISHED
 *
 * Established: this host boots, services NMI through the real 00:80B2, paces
 * audio, presents video, and passes --qualify at 600 frames.
 *
 * NOT established: that it is EQUIVALENT to the per-opcode host. --qualify is
 * an activity bar, not an equivalence bar, and the two hosts do not agree on
 * their counters (logic_changes 425 vs 592 at 600 frames). Some of that is
 * expected -- the frame host advances the guest in coarser units -- but none
 * of it has been shown harmless. Treat the per-opcode path as the correctness
 * baseline until a differential run says otherwise.
 *
 * Also note: the M/X and PC bitmaps are recorded in the per-opcode loop only,
 * so a fiber run records no coverage (banks_seen=0). Every coverage tool in
 * tools/ therefore still needs the interpreter host.
 *
 * Separate translation unit on purpose: this needs cpu_state.h, and that
 * header declares a global `CpuState g_cpu` which collides with src/main.c's
 * `Interp816 *g_cpu`.
 */
#include <stdio.h>
#include <stdlib.h>

#include "cpu_state.h"
#include "common_rtl.h"
#include "snes/snes.h"
#include "snes/interp_bridge.h"
#include "simcity_fiberdrive.h"
#include "interp816.h"

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
#define SC_NMI_VECTOR  0xFFEAu   /* 65816 native NMI vector; this ROM: 00:80B2 */

/* Beam advance lives in src/main.c, which owns the device model. */
void sc_advance_beam_one_frame(void);
void sc_advance_until_input_ready(void);

static CpuState s_cpu;
static bool     s_started;
static uint32   s_resume_pc24;
static unsigned s_bail_streak;
static bool     s_ran_once;
static uint64_t s_nmi_delivered;

/* Runtime globals the AOT bus path needs. src/main.c builds its own Snes via
 * snes_init() and never calls SnesInit(), which is where common_cpu_infra.c
 * normally publishes these. It happens to set g_ppu and nothing else, so
 * g_dma / g_snes_cpu / g_rom stay NULL -- harmless while every access goes
 * through the interpreter, fatal the moment the bridge routes a hardware
 * write through WriteReg: $4300 lands in dma_write(g_dma, ...) with g_dma
 * NULL and never returns. That was the frame-2 wedge.
 *
 * Publishing them here rather than in main.c keeps the ordinary interpreter
 * build byte-for-byte unchanged. */
extern Snes *g_snes;

static void publish_runtime_globals(void) {
    if (!g_snes) return;
    g_dma  = g_snes->dma;
    g_ppu  = g_snes->ppu;
    if (g_snes->cart) g_rom = g_snes->cart->rom;
}

bool SimCityFiberDrive_Init(void) {
    if (s_started) return true;
    cpu_state_init(&s_cpu, g_ram);
    /* 65816 reset contract: native mode, 8-bit A and index, stack in page 1. */
    s_cpu.PB = 0x00; s_cpu.DB = 0x00; s_cpu.D = 0x0000;
    s_cpu.S = 0x01ff;
    s_cpu.m_flag = 1; s_cpu.x_flag = 1;
    s_cpu.emulation = 0;
    s_cpu.P = 0x30;
    cpu_p_to_mirrors(&s_cpu);
    s_cpu.host_return_valid = 0;
    s_resume_pc24 = SC_RESET_PC24;
    s_started = true;
    return true;
}

/* Adopt a loaded save state.
 *
 * load_state() restores g_snes and the INTERP816 cpu; it knows nothing about
 * this file's `static CpuState s_cpu`, which is what the fiber actually
 * executes. Worse, Init() runs during env parsing -- long before the state is
 * loaded -- and pins the 65816 reset contract: PB=0, DB=0, D=0, S=$01ff, 8-bit
 * A and index, resume at the reset vector.
 *
 * So without this the fiber ran boot registers over mid-game WRAM: measured as
 * a hang at $05935A within 60 frames, spinning on a BNE in the block-copy
 * region, while the same state loaded fine on the interpreter and the fiber
 * booted fine without a state.
 *
 * Copies the architectural registers across and republishes the resume PC.
 * Nothing else in CpuState is guest-visible state -- host_return_valid is the
 * paired-call bookkeeping and must start clean, exactly as after Init. */
void SimCityFiberDrive_AdoptInterpState(const Interp816 *in) {
    if (!s_started || !in) return;
    s_cpu.A  = in->a;
    s_cpu.X  = in->x;
    s_cpu.Y  = in->y;
    s_cpu.S  = in->sp;
    s_cpu.D  = in->dp;
    s_cpu.PB = in->k;
    s_cpu.DB = in->db;
    s_cpu.m_flag = in->mf ? 1 : 0;
    s_cpu.x_flag = in->xf ? 1 : 0;
    s_cpu.emulation = in->e ? 1 : 0;
    s_cpu.P = (uint8_t)((in->c ? 0x01 : 0) | (in->z ? 0x02 : 0) |
                        (in->i ? 0x04 : 0) | (in->d ? 0x08 : 0) |
                        (in->xf ? 0x10 : 0) | (in->mf ? 0x20 : 0) |
                        (in->v ? 0x40 : 0) | (in->n ? 0x80 : 0));
    cpu_p_to_mirrors(&s_cpu);
    s_cpu.host_return_valid = 0;
    s_resume_pc24 = ((uint32_t)in->k << 16) | in->pc;
}

bool SimCityFiberDrive_RunGuestSlice(uint64_t frame, bool nmi_pending,
                                     uint64_t budget) {
    /* Do not hand over a frame whose input latch is still busy -- see the long
     * note on sc_advance_until_input_ready() in src/main.c. */
    /* First frame: g_snes does not exist yet when Init() runs (that happens
     * during env parsing, before snes_init), so publish here instead. */
    { static int published = 0;
      if (!published) { published = 1; publish_runtime_globals(); } }

    const int trace = getenv("SC_FRAME_TRACE") != NULL;
    if (trace) fprintf(stderr, "[frame %llu] a: draining input latch\n",
                       (unsigned long long)frame);
    sc_advance_until_input_ready();
    if (trace) fprintf(stderr, "[frame %llu] b: latch drained, bridge at %06X\n",
                       (unsigned long long)frame, (unsigned)s_resume_pc24);

    /* ── Deliver NMI the way the hardware does ────────────────────────────
     *
     * The host raised this NMI during the beam advance above, but it raised
     * it on `g_cpu`, the Interp816 -- which does not run at all in fiber
     * mode. So the request was being set on a CPU that never executes,
     * `nmi_serviced` read 0 forever, and the driver faked the handler's
     * effect with a direct `g_ram[$b9] = 1`. That released 00:930d's wait but
     * skipped the whole of 00:80B2, which on this game is the per-frame PPU
     * work -- hence a guest that computed (`logic_changes` moving) and a
     * screen that never changed (`video_changes=0`).
     *
     * interp_bridge_run_interrupt() is the runtime's entry for this, and the
     * earlier attempt to use it stalled the guest completely (logic_changes
     * 298 -> 0, master 173M -> 897M). The header says why: "the caller has
     * already materialized the hardware interrupt frame." It had not. The
     * handler's terminal RTI therefore popped whatever happened to be under
     * S, and the run never came back to anywhere useful.
     *
     * That nested entry is not needed here anyway. Materializing the frame
     * ourselves and pointing the resume PC at the vector makes the handler
     * simply the next thing the ordinary run_loop executes: it runs 00:80B2,
     * does its own INC $b9 at 00:80bc, and RTIs back to the interrupted PC --
     * the vblank spin, now released. One bridge entry per frame, exactly as
     * before, and no faked WRAM write.
     *
     * Not on the first frame: s_resume_pc24 is still the RESET vector there,
     * and pushing a return frame for code that has never run would have the
     * handler RTI straight into it. */
    if (nmi_pending && s_ran_once) {
        uint32 ret = s_resume_pc24;
        cpu_push_interrupt_frame_at(&s_cpu, ret);
        s_cpu.P |= 0x04;              /* I: interrupts off inside the handler */
        s_cpu.P &= (uint8)~0x08;      /* D: hardware clears decimal on entry  */
        cpu_p_to_mirrors(&s_cpu);
        s_cpu.PB = 0x00;
        s_resume_pc24 = cpu_read16(&s_cpu, 0x00, SC_NMI_VECTOR);
        s_nmi_delivered++;
        if (trace) fprintf(stderr, "[frame %llu] nmi -> %06X (ret %06X)\n",
                           (unsigned long long)frame,
                           (unsigned)s_resume_pc24, (unsigned)ret);
    }

    /* ARM THE EXECUTION BOUND. The bridge's step cap counts *interpreted*
     * steps only; once it bounces into a compiled body, nothing counts. Every
     * generated block polls interp_bridge_lle_master_deadline_reached(), but
     * that returns false unless a deadline is set, so an unbounded compiled
     * loop -- e.g. the 00:9280 $4212 spin as AOT code -- hangs the host with no
     * step cap to catch it. One frame of master cycles is the natural bound. */
    /* One frame. Tested at 600 frames too: the run behaves identically, so the
     * deadline is NOT what stops it -- see MIGRATION_step3 §10. */
    /* The caller sizes the budget so the BEAM CANNOT CROSS THE FRAME BOUNDARY
     * while the guest holds the CPU.
     *
     * A flat one-frame bound was the original defect. The bridge advances the
     * beam by the guest's own master cycles as it executes, so a full-frame
     * budget let vPos wrap MID-BURST and the frame was presented from inside
     * the guest's update. Resizing it does not help -- both directions measure
     * strictly worse (docs/TODO_fiber_rendering.md has the sweep) -- because
     * the bound is an open-loop guess at where the beam will end up. The
     * caller computes it from the actual beam position instead. */
    interp_bridge_set_master_deadline(s_cpu.master_cycles + budget);

    unsigned long hle_before = g_simcity_vblank_hle_calls;
    int ok = interp_bridge_run_loop(&s_cpu, s_resume_pc24,
                                    SC_YIELD_PC24,
                                    SC_VBLANK_FLAG,
                                    SC_VBLANK_WAITING);
    if (trace) fprintf(stderr, "[frame %llu] c: bridge returned ok=%d\n",
                       (unsigned long long)frame, ok);
    s_ran_once = true;
    uint32 resume = interp_bridge_lle_resume_pc();
    if (resume) s_resume_pc24 = resume;

    if (!ok) {
        /* The bridge hit its step cap instead of reaching the wait. One of
         * these is survivable (a long init frame); a run of them is not. */
        if (++s_bail_streak > 30) {
            fprintf(stderr, "[frame] bridge bailed %u frames running at "
                    "frame %llu (resume=%06X, vblank hle calls=%lu)\n",
                    s_bail_streak, (unsigned long long)frame,
                    (unsigned)s_resume_pc24, g_simcity_vblank_hle_calls);
            return false;
        }
    } else {
        s_bail_streak = 0;
    }
    (void)hle_before;
    return true;
}

/* The qualify counters and the APU pacing in src/main.c are driven from
 * g_master_cycles, which only the per-opcode loop increments. Expose the
 * guest clock so the frame path can advance it by the same amount the guest
 * actually consumed. */
uint64_t SimCityFiberDrive_MasterCycles(void) { return s_cpu.master_cycles; }

/* NMIs actually delivered to the guest, for the qualify bar's nmi_serviced. */
uint64_t SimCityFiberDrive_NmiDelivered(void) { return s_nmi_delivered; }

/* Guest stack pointer and resume PC, for host-vs-host comparisons: the two
 * hosts sample WRAM at different points in the guest's frame, so knowing
 * where the guest actually is decides whether a differing byte is live state
 * or dead stack below S. */
unsigned SimCityFiberDrive_GuestS(void)  { return (unsigned)s_cpu.S; }
unsigned SimCityFiberDrive_ResumePC(void){ return (unsigned)s_resume_pc24; }
