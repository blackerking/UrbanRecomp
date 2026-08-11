/*
 * AOT link probe -- step 1 of the CpuState migration.
 *
 * This is deliberately NOT the game. It links the generated banks
 * (src/gen/*.c) against the shared AOT/CpuState runtime (cpu_state.c,
 * common_cpu_infra.c, common_rtl.c, interp_bridge.c) and does nothing but
 * report what came out. Its whole job is to answer, cheaply and without
 * risking the working build, a question nobody in this project has actually
 * tested: *does the emitted C compile and link against this runtime at all?*
 *
 * Until now `src/gen/*.c` has been generated on every regen and never built
 * (see the comment in CMakeLists.txt). 312k lines of never-compiled code is
 * a lot of unverified assumption to migrate the real host onto in one step,
 * so this target goes first. It also flushes out the g_ram collision the
 * CMakeLists comment warned about: common_rtl.c defines `uint8 g_ram[0x20000]`
 * and src/main.c defines its own identically-sized array, which is why the
 * two cannot currently be linked together. This probe links common_rtl.c's
 * and leaves main.c alone.
 *
 * Build: cmake --build build --target SimCityAOTProbe
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "cpu_state.h"
#include "common_cpu_infra.h"
#include "spc_player.h"
#include "snes/dsp.h"
#include "snes/dsp_shadow.h"

/* Host-provided symbols the shared runtime expects the game's own main to
 * supply. The real desktop hosts define these in their host_main include;
 * the probe only needs them to satisfy the linker.
 *
 * RtlApuLock/Unlock guard the APU across the runner's audio thread. This
 * probe never starts audio and never runs guest code, so no-ops are correct
 * here -- but the real migration must provide genuine locking, because
 * src/main.c drives the DSP itself. */
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

/* debug_server.h declares these write hooks and calls them from inline
 * helpers, but debug_server.c cannot be compiled alongside its own header in
 * this configuration (the header already carries bodies for a dozen of its
 * entry points, so linking the .c gives C2084 redefinitions). The hooks are
 * pure observability, so no-ops are the correct stub. */
void debug_on_wram_write_byte(uint32_t addr, uint8_t old_val, uint8_t new_val)
    { (void)addr; (void)old_val; (void)new_val; }
void debug_on_wram_write_word(uint32_t addr, uint16_t old_val, uint16_t new_val)
    { (void)addr; (void)old_val; (void)new_val; }
void debug_on_block_enter(uint32_t pc, uint32_t a, uint32_t x, uint32_t y)
    { (void)pc; (void)a; (void)x; (void)y; }

/* src/main.c defines these same three for the interpreter-tier build; the
 * probe is a separate link unit so it needs its own copies. When the real
 * migration merges the two, these move out of both and into one place. */
void Die(const char *e) { fprintf(stderr, "FATAL: %s\n", e ? e : "(null)"); exit(1); }
SpcPlayer *g_spc_player = NULL;

/* Same shape as src/main.c's: dsp_shadow.c drags in a shadow_verifier that
 * this project does not build, so the shadow DSP is stubbed to pass-through. */
DspShadow *dsp_shadow_create(void) { return NULL; }
void dsp_shadow_free(DspShadow *sh) { (void)sh; }
void dsp_shadow_process(DspShadow *sh, Dsp *dsp, int cL, int cR, int *oL, int *oR) {
  (void)sh; (void)dsp; *oL = cL; *oR = cR;
}
void dsp_shadow_verify_brr(const uint8_t *aram, uint16_t bs, int a, int b, const int16_t *c) {
  (void)aram; (void)bs; (void)a; (void)b; (void)c;
}
void dsp_shadow_verify_echo(const int16_t *l, const int16_t *r, const int8_t *co,
                            int idx, int sL, int sR) {
  (void)l; (void)r; (void)co; (void)idx; (void)sL; (void)sR;
}

int main(void) {
    unsigned rows = g_dispatch_table_count;
    unsigned with_body = 0, bodies = 0, lle_rows = 0;
    unsigned per_variant[4] = { 0, 0, 0, 0 };

    for (unsigned i = 0; i < rows; i++) {
        int any = 0;
        for (int v = 0; v < 4; v++) {
            if (g_dispatch_table[i].variant[v]) {
                bodies++;
                per_variant[v]++;
                any = 1;
            }
        }
        if (any) with_body++; else lle_rows++;
    }

    printf("aot_probe: linked OK\n");
    printf("  dispatch rows            : %u\n", rows);
    printf("  rows with >=1 AOT body   : %u\n", with_body);
    printf("  rows entirely LLE        : %u\n", lle_rows);
    printf("  total compiled variants  : %u\n", bodies);
    printf("  per (m,x): M0X0=%u M0X1=%u M1X0=%u M1X1=%u\n",
           per_variant[0], per_variant[1], per_variant[2], per_variant[3]);
    printf("  ram-routine guards       : %u\n", g_ram_routine_guard_count);
    return 0;
}
