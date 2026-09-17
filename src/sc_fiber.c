/*
 * Fiber layer for the AOT frame boundary.
 *
 * Chosen design (see docs/MIGRATION_step3.md §4): follow ar-recomp and give
 * the game its own coroutine, so the vblank HLE can hand a frame back to the
 * host from arbitrary call depth. `snesrecomp/docs/LLE_SCHEDULER.md` solves
 * the same problem with an NLR unwind instead; a fiber is the older approach
 * but it is the one with a working reference implementation on this runtime.
 *
 * WHY THIS IS NOT A STRAIGHT TRANSPLANT OF ar-recomp
 *
 * ar-recomp's game coroutine begins by calling its compiled reset handler
 * (`ResetHandler_M1X1(&g_cpu)`) and never returns -- the entire game runs as
 * compiled C inside that one call, and the fiber exists purely to suspend it.
 *
 * The game cannot start that way. Both architectural entry points are
 * `lle_only`:
 *
 *   I_RESET   00:8000  unproven_call_at_008056_to_03D283  (mode dispatcher)
 *   NMI       00:80b2  unproven_call_at_00813A_to_00C3F9
 *
 * `03:d283` is the screen-mode dispatcher -- `LDA $14 ; ASL ; TAX ;
 * JMP ($d255,X)` -- so its exit modes are unprovable through the indirect
 * jump, and that single unproven call leaves the reset vector uncompiled.
 * There is no compiled entry point to hand the fiber.
 *
 * So the fiber wraps an INTERPRETER that bounces into compiled bodies, rather
 * than hosting compiled code directly. That is a hybrid of the two designs,
 * and it is the honest shape for a game where 41% of executed addresses have
 * no compiled body at all.
 *
 * This file provides only the coroutine mechanics and the yield the HLE calls.
 * Driving the guest from inside it is the next step and is deliberately not
 * done here.
 */
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <ucontext.h>
#endif

#include "sc_fiber.h"

#ifdef _WIN32
static LPVOID s_host_fiber;
static LPVOID s_game_fiber;
static int    s_converted_thread;
#else
static ucontext_t s_host_ctx;
static ucontext_t s_game_ctx;
static char       s_game_stack[2 * 1024 * 1024];
#endif

static ScFiberEntry s_entry;
static int s_created;
unsigned long g_sc_fiber_yields;

#ifdef _WIN32
static VOID CALLBACK sc_fiber_trampoline(LPVOID param) {
    (void)param;
    if (s_entry) s_entry();
    /* The entry must never return: there is no host frame to return into, and
     * on Windows a fiber routine that returns terminates the thread. Park
     * here yielding forever, which is what ar-recomp's game_coroutine does. */
    for (;;) ScFiber_YieldToHost();
}
#else
static void sc_fiber_trampoline(void) {
    if (s_entry) s_entry();
    for (;;) ScFiber_YieldToHost();
}
#endif

int ScFiber_Create(ScFiberEntry entry) {
    if (s_created) return 1;
    s_entry = entry;
#ifdef _WIN32
    if (!s_converted_thread) {
        /* ConvertThreadToFiberEx with FIBER_FLAG_FLOAT_SWITCH: without it the
         * x87/SSE state is NOT switched between fibers, and this host does use
         * floating point on both sides of the boundary (the DSP resample phase
         * and the frame pacing). ar-recomp documents the same requirement. */
        s_host_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
        if (!s_host_fiber) {
            /* Already a fiber (a second Create, or an embedding host). */
            s_host_fiber = GetCurrentFiber();
            if (!s_host_fiber) return 0;
        }
        s_converted_thread = 1;
    }
    /* Reserve 2MB, commit 64KB. The recompiled dispatch stack can nest deeply,
     * so the reserve must be generous, but committing it all up front makes
     * (re)creation expensive. */
    s_game_fiber = CreateFiberEx(64 * 1024, 2 * 1024 * 1024,
                                 FIBER_FLAG_FLOAT_SWITCH,
                                 sc_fiber_trampoline, NULL);
    if (!s_game_fiber) return 0;
#else
    if (getcontext(&s_game_ctx) != 0) return 0;
    s_game_ctx.uc_stack.ss_sp = s_game_stack;
    s_game_ctx.uc_stack.ss_size = sizeof(s_game_stack);
    s_game_ctx.uc_link = NULL;
    makecontext(&s_game_ctx, sc_fiber_trampoline, 0);
#endif
    s_created = 1;
    return 1;
}

void ScFiber_Destroy(void) {
    if (!s_created) return;
#ifdef _WIN32
    if (s_game_fiber) { DeleteFiber(s_game_fiber); s_game_fiber = NULL; }
#endif
    s_created = 0;
}

int ScFiber_Created(void) { return s_created; }

void ScFiber_RunOneFrame(void) {
    if (!s_created) return;
#ifdef _WIN32
    SwitchToFiber(s_game_fiber);
#else
    if (swapcontext(&s_host_ctx, &s_game_ctx) != 0) {
        fprintf(stderr, "FATAL: swapcontext (host -> game) failed\n");
        abort();
    }
#endif
}

void ScFiber_YieldToHost(void) {
    g_sc_fiber_yields++;
#ifdef _WIN32
    SwitchToFiber(s_host_fiber);
#else
    /* An unchecked swapcontext failure would silently return and keep running
     * on a stack the host believes it owns, with no recovery. Abort loudly. */
    if (swapcontext(&s_game_ctx, &s_host_ctx) != 0) {
        fprintf(stderr, "FATAL: swapcontext (game -> host) failed\n");
        abort();
    }
#endif
}
