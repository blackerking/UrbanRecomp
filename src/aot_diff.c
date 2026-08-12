/*
 * AOT differential harness -- migration step 3a.
 *
 * Steps 1 and 2 proved the generated code builds, links, and can share a
 * binary with the interpreter. None of that executes a single compiled
 * instruction, so none of it says the emitted C is *correct*. This runs one
 * compiled body and the real ROM routine over identical inputs and compares
 * the resulting guest state.
 *
 * The subject is 00:824f, the game's PRNG step (see docs/ROM_MAP.md):
 *
 *     REP #$20 ; CLC ; LDA $59 ; STA $5d ; ADC $5b ; STA $59 ; ADC $5d
 *     STA $5b ; RTS
 *
 * chosen deliberately as the easiest possible case: pure WRAM state, no I/O,
 * no branches, no calls, and a 16-bit-wide body entered at a known width. If
 * the emitted C is wrong *here*, nothing further is worth attempting; if it
 * is right, that is one verified data point and not a claim about the other
 * 719 variants.
 *
 * Method: seed $59/$5b/$5d, run one side, snapshot; restore the seed, run the
 * other side, snapshot; compare. The interpreter side runs from a fresh
 * interp816 stepped until PC leaves the routine's extent, which is exact for
 * a straight-line routine like this one.
 *
 * Build: cmake --build build --target SimCityAOTDiff
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "snes/snes.h"
#include "snes/interp816.h"
#include "cpu_state.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "spc_player.h"
#include "snes/dsp.h"
#include "snes/dsp_shadow.h"

/* Host symbols the shared runtime expects the game to supply. */
SpcPlayer *g_spc_player = NULL;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void debug_on_wram_write_byte(uint32_t a, uint8_t o, uint8_t n)
  { (void)a; (void)o; (void)n; }
void debug_on_wram_write_word(uint32_t a, uint16_t o, uint16_t n)
  { (void)a; (void)o; (void)n; }
void debug_on_block_enter(uint32_t pc, uint32_t a, uint32_t x, uint32_t y)
  { (void)pc; (void)a; (void)x; (void)y; }
void Die(const char *e) { fprintf(stderr, "FATAL: %s\n", e ? e : "(null)"); exit(1); }
DspShadow *dsp_shadow_create(void) { return NULL; }
void dsp_shadow_free(DspShadow *sh) { (void)sh; }
void dsp_shadow_process(DspShadow *sh, Dsp *dsp, int cL, int cR, int *oL, int *oR)
  { (void)sh; (void)dsp; *oL = cL; *oR = cR; }
void dsp_shadow_verify_brr(const uint8_t *a, uint16_t b, int c, int d, const int16_t *e)
  { (void)a; (void)b; (void)c; (void)d; (void)e; }
void dsp_shadow_verify_echo(const int16_t *l, const int16_t *r, const int8_t *c,
                            int i, int sL, int sR)
  { (void)l; (void)r; (void)c; (void)i; (void)sL; (void)sR; }

Snes *g_snes;
Ppu  *g_ppu;

static uint8_t bus_read(void *mem, uint32_t adr) { (void)mem; return snes_read(g_snes, adr); }
static void    bus_write(void *mem, uint32_t adr, uint8_t v) { (void)mem; snes_write(g_snes, adr, v); }

#define PRNG_PC24   0x00824Fu
#define PRNG_END    0x008260u   /* past the RTS */

typedef struct { uint16_t s59, s5b, s5d, A; } Snapshot;

static void seed(uint16_t a, uint16_t b, uint16_t c) {
    g_ram[0x59] = (uint8_t)a; g_ram[0x5a] = (uint8_t)(a >> 8);
    g_ram[0x5b] = (uint8_t)b; g_ram[0x5c] = (uint8_t)(b >> 8);
    g_ram[0x5d] = (uint8_t)c; g_ram[0x5e] = (uint8_t)(c >> 8);
}
static Snapshot snap(uint16_t A) {
    Snapshot s;
    s.s59 = (uint16_t)(g_ram[0x59] | (g_ram[0x5a] << 8));
    s.s5b = (uint16_t)(g_ram[0x5b] | (g_ram[0x5c] << 8));
    s.s5d = (uint16_t)(g_ram[0x5d] | (g_ram[0x5e] << 8));
    s.A = A;
    return s;
}

/* Find the compiled body for a pc24, preferring the widest available
 * variant. Returns NULL if every variant of this row is LLE. */
static RecompReturn (*find_body(uint32_t pc24, int *which))(CpuState *) {
    for (unsigned i = 0; i < g_dispatch_table_count; i++) {
        if (g_dispatch_table[i].pc24 != pc24) continue;
        for (int v = 0; v < 4; v++)
            if (g_dispatch_table[i].variant[v]) { *which = v; return g_dispatch_table[i].variant[v]; }
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *rom_path = argc > 1 ? argv[1] : "simcity.sfc";
    FILE *f = fopen(rom_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", rom_path); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *rom = (uint8_t *)malloc((size_t)sz);
    if (fread(rom, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 2; }
    fclose(f);

    g_snes = snes_init(g_ram);
    if (!snes_loadRom(g_snes, rom, (int)sz)) { fprintf(stderr, "rom load failed\n"); return 2; }
    snes_reset(g_snes, true);
    g_ppu = g_snes->ppu;

    int which = -1;
    RecompReturn (*body)(CpuState *) = find_body(PRNG_PC24, &which);
    if (!body) { fprintf(stderr, "no compiled body for %06X\n", PRNG_PC24); return 1; }
    static const char *vn[4] = { "M0X0", "M0X1", "M1X0", "M1X1" };
    printf("subject 00:824f (PRNG step), compiled variant %s\n\n", vn[which]);

    Interp816 *icpu = interp816_init(NULL, bus_read, bus_write);
    interp816_reset(icpu);

    static const uint16_t cases[][3] = {
        { 0x0000, 0x0000, 0x0000 }, { 0x0001, 0x0000, 0x0000 },
        { 0x1234, 0x5678, 0x9abc }, { 0xffff, 0xffff, 0xffff },
        { 0x8000, 0x8000, 0x0000 }, { 0xabcd, 0x0001, 0xf00d },
        { 0x0f0f, 0xf0f0, 0x00ff }, { 0x7fff, 0x0001, 0x0000 },
    };
    int n = (int)(sizeof(cases) / sizeof(cases[0])), bad = 0;

    printf("%-22s %-24s %-24s\n", "seed 59/5b/5d", "AOT 59/5b/5d (A)", "interp 59/5b/5d (A)");
    for (int i = 0; i < n; i++) {
        uint16_t a = cases[i][0], b = cases[i][1], c = cases[i][2];

        /* --- compiled side --- */
        seed(a, b, c);
        CpuState cs;
        cpu_state_init(&cs, g_ram);
        cs.PB = 0x00; cs.DB = 0x00; cs.D = 0x0000; cs.S = 0x1ff3;
        cs.A = 0; cs.X = 0; cs.Y = 0;
        cs.m_flag = 1; cs.x_flag = 1; cs.emulation = 0;
        cs.P = 0x30; cpu_p_to_mirrors(&cs);
        /* Option-1 ABI: a JSR-paired caller pushes a 2-byte return frame and
         * declares it, so the body's RTS may return NORMAL (cpu_state.h). */
        cs.ram[0x1ff3] = 0x34; cs.ram[0x1ff2] = 0x12;
        cs.S = 0x1ff1;
        cs.host_return_valid = 2;
        RecompReturn r = body(&cs);
        Snapshot got = snap(cs.A);

        /* --- interpreter side, same seed --- */
        seed(a, b, c);
        icpu->k = 0x00; icpu->pc = (uint16_t)PRNG_PC24;
        icpu->a = 0; icpu->x = 0; icpu->y = 0;
        icpu->dp = 0; icpu->db = 0; icpu->sp = 0x1ff1;
        icpu->mf = 1; icpu->xf = 1; icpu->e = 0;
        bus_write(NULL, 0x1ff3, 0x34); bus_write(NULL, 0x1ff2, 0x12);
        for (int guard = 0; guard < 64; guard++) {
            uint32_t pc = ((uint32_t)icpu->k << 16) | icpu->pc;
            if (pc < PRNG_PC24 || pc >= PRNG_END) break;
            interp816_runOpcode(icpu);
        }
        Snapshot want = snap(icpu->a);

        int ok = got.s59 == want.s59 && got.s5b == want.s5b && got.s5d == want.s5d;
        if (!ok) bad++;
        printf("%04x %04x %04x   ->  %04x %04x %04x (%04x)   %04x %04x %04x (%04x)  %s%s\n",
               a, b, c,
               got.s59, got.s5b, got.s5d, got.A,
               want.s59, want.s5b, want.s5d, want.A,
               ok ? "MATCH" : "*** MISMATCH ***",
               r == RECOMP_RETURN_NORMAL ? "" : " [body did not return NORMAL]");
    }

    printf("\n%d/%d cases match\n", n - bad, n);
    return bad ? 1 : 0;
}
