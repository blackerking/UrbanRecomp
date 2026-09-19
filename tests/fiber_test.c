/*
 * Self-test for the fiber layer (src/sc_fiber.c).
 *
 * Coroutine bugs are miserable to debug once a 65816 interpreter is running
 * on top of them -- a corrupted stack or unswitched FP state shows up as
 * inexplicable guest misbehaviour a long way from the cause. So the mechanics
 * get tested on their own first: does control alternate correctly, is the
 * game stack preserved across a switch, and does floating-point state survive
 * (the reason FIBER_FLAG_FLOAT_SWITCH is mandatory)?
 */
#include <stdio.h>
#include <string.h>

#include "sc_fiber.h"

static int   s_frames_seen;
static int   s_stack_corrupt;
static double s_fp_in_game;

/* Recurse a little before yielding, so the test proves the game stack really
 * is preserved across a switch rather than just the entry frame. Each level
 * re-checks its own local AFTER the resume: if a switch corrupted the game
 * stack, some level's marker would come back wrong. */
static void deep(int depth) {
    volatile int marker = depth * 7 + 1;
    if (depth > 0) {
        deep(depth - 1);
    } else {
        s_frames_seen++;
        s_fp_in_game = s_fp_in_game * 1.5 + 0.25;
        ScFiber_YieldToHost();   /* suspend from 8 frames down */
    }
    if (marker != depth * 7 + 1) s_stack_corrupt = 1;
}

static void game_entry(void) {
    for (;;) deep(8);
}

int main(void) {
    int fails = 0;

    if (!ScFiber_Create(game_entry)) {
        printf("FAIL: could not create fiber\n");
        return 1;
    }
    printf("fiber created\n");

    double host_fp = 3.25;
    s_fp_in_game = 1.0;

    for (int i = 1; i <= 5; i++) {
        host_fp = host_fp * 2.0 - 0.5;      /* host FP work around the switch */
        ScFiber_RunOneFrame();
        if (s_frames_seen != i) {
            printf("FAIL: frame %d -> game ran %d times\n", i, s_frames_seen);
            fails++;
        }
        if (s_stack_corrupt) {
            printf("FAIL: frame %d -> a game stack local did not survive "
                   "the switch\n", i);
            fails++;
        }
    }

    /* 3.25 doubled-minus-a-half five times. Computed here rather than
     * hardcoded so the check is about FP state surviving the switches, not
     * about my arithmetic. */
    double want = 3.25;
    for (int i = 0; i < 5; i++) want = want * 2.0 - 0.5;
    if (host_fp != want) {
        printf("FAIL: host FP %f, want %f (FIBER_FLAG_FLOAT_SWITCH?)\n",
               host_fp, want);
        fails++;
    }
    double wantg = 1.0;
    for (int i = 0; i < 5; i++) wantg = wantg * 1.5 + 0.25;
    if (s_fp_in_game != wantg) {
        printf("FAIL: game FP %f, want %f\n", s_fp_in_game, wantg);
        fails++;
    }

    printf("  frames driven      : %d\n", s_frames_seen);
    printf("  yields recorded    : %lu\n", g_sc_fiber_yields);
    printf("  game stack intact  : %s\n", s_stack_corrupt ? "NO" : "yes");
    printf("  host FP preserved  : %s\n", host_fp == want ? "yes" : "NO");
    printf("  game FP preserved  : %s\n", s_fp_in_game == wantg ? "yes" : "NO");

    if (g_sc_fiber_yields != 5) {
        printf("FAIL: %lu yields, want 5\n", g_sc_fiber_yields);
        fails++;
    }

    ScFiber_Destroy();
    printf("%s\n", fails ? "FIBER TEST FAILED" : "fiber test PASSED");
    return fails ? 1 : 0;
}
