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
 * Build: cmake --build build --target UrbanRecompAOTDiff
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
#include "pure_leaves.h"

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
    if (argc < 2) { fprintf(stderr, "usage: %s <US ROM>\n", argv[0]); return 2; }
    const char *rom_path = argv[1];
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

    Interp816 *icpu = interp816_init(NULL, bus_read, bus_write);
    interp816_reset(icpu);

    /* Scratch WRAM the routines operate on. Saved/restored around each side so
     * both see identical input; the low 2KB covers direct page and the
     * near-page variables these leaf routines touch. */
    static uint8_t saved[0x2000];
    uint32_t rng = 0x12345678u;
    #define NEXT() (rng = rng * 1664525u + 1013904223u, (uint16_t)(rng >> 16))

    int tested = 0, matched = 0, skipped = 0, nonnormal = 0, nonterm = 0;
    int cycle_match = 0, cycle_mismatch = 0;
    int failing_pcs = 0;
    unsigned first_fail[8]; int nfail = 0;

    for (int i = 0; i < SC_PURE_LEAF_COUNT; i++) {
        unsigned pc24 = kScPureLeaves[i].pc24;
        unsigned end  = kScPureLeaves[i].end;
        int which = -1;
        RecompReturn (*body_fn)(CpuState *) = find_body(pc24, &which);
        if (!body_fn) { skipped++; continue; }

        printf("  [%2d/%d] %02X:%04X ...\n", i + 1, SC_PURE_LEAF_COUNT,
               pc24 >> 16, pc24 & 0xffff);
        fflush(stdout);
        int body_bad = 0, body_skip = 0;
        for (int trial = 0; trial < 8 && !body_skip; trial++) {
            /* randomise the scratch region and the entry registers */
            /* Values are kept SMALL on purpose. Many of these routines take a
             * table index out of a WRAM variable -- 01:b375 does
             * `LDX $01f9 ; LDA $0180c0,X` -- so feeding fully random WRAM
             * hands them a wild index and produces an out-of-range access that
             * looks like a codegen bug but is really a violated precondition.
             * Real callers keep those indices small; so do we. Diversity comes
             * from the value pattern, not from magnitude. */
            for (int a = 0; a < 0x2000; a++) saved[a] = (uint8_t)(NEXT() & 0x0f);
            uint16_t rA = (uint16_t)(NEXT() & 0x0fff);
            uint16_t rX = (uint16_t)(NEXT() & 0x000f);
            uint16_t rY = (uint16_t)(NEXT() & 0x000f);
            uint8_t  rDB = 0x00, mf = which >= 2, xf = (which & 1);

            /* INTERPRETER FIRST, and this ordering is load-bearing. A compiled
             * body is a plain C call with no way to interrupt it, so a routine
             * that does not terminate hangs the harness with no diagnostic --
             * which is exactly what 00:930d does, since it is the vblank spin
             * (STZ $b9 ; INC $c7 ; LDA $b9 ; BEQ -6) waiting on an NMI that
             * never arrives here. The interpreter is stepped under a guard, so
             * running it first lets us detect non-termination and skip the
             * body entirely rather than wedging. */
            memcpy(g_ram, saved, 0x2000);
            icpu->k = (uint8_t)(pc24 >> 16); icpu->pc = (uint16_t)pc24;
            icpu->a = rA; icpu->x = rX; icpu->y = rY;
            icpu->dp = 0; icpu->db = rDB; icpu->sp = 0x1ff1;
            icpu->mf = mf; icpu->xf = xf; icpu->e = 0;
            bus_write(NULL, 0x1ff3, 0x34); bus_write(NULL, 0x1ff2, 0x12);
            /* A clean exit means the routine RETURNED -- its RTS popped the
             * frame we pushed and landed on $3413. Merely leaving [pc24, end]
             * is not the same thing: a manifest extent can stop short of the
             * real routine (00:8436's ends at $8448, one byte before its own
             * RTS, because the not-equal path continues into a separate node),
             * and in that case the interpreter would stop early while the
             * compiled body carried on -- a divergence entirely of the
             * harness's making. Anything that does not return cleanly is
             * skipped rather than compared. */
            int returned = 0;
            long icycles = 0;
            for (int guard = 0; guard < 4096; guard++) {
                uint32_t p = ((uint32_t)icpu->k << 16) | icpu->pc;
                if (p == 0x003413u) { returned = 1; break; }
                if (p < pc24 || p > end + 1) break;
                { int c = interp816_runOpcode(icpu); icycles += (c > 0 ? c : 1) * 8; }
            }
            if (!returned) { body_skip = 1; break; }
            static uint8_t after_interp[0x2000];
            memcpy(after_interp, g_ram, 0x2000);
            uint16_t iA = icpu->a, iX = icpu->x, iY = icpu->y;

            memcpy(g_ram, saved, 0x2000);
            CpuState cs;
            cpu_state_init(&cs, g_ram);
            cs.PB = (uint8_t)(pc24 >> 16); cs.DB = rDB; cs.D = 0x0000;
            cs.A = rA; cs.X = rX; cs.Y = rY;
            cs.m_flag = mf; cs.x_flag = xf; cs.emulation = 0;
            cs.P = (uint8_t)((mf ? 0x20 : 0) | (xf ? 0x10 : 0));
            cpu_p_to_mirrors(&cs);
            cs.ram[0x1ff3] = 0x34; cs.ram[0x1ff2] = 0x12;
            cs.S = 0x1ff1; cs.host_return_valid = 2;
            uint64_t mc_before = cs.master_cycles;
            RecompReturn r = body_fn(&cs);
            if (r != RECOMP_RETURN_NORMAL) nonnormal++;
            {
                long acycles = (long)(cs.master_cycles - mc_before);
                if (acycles != icycles) {
                    if (cycle_mismatch < 6)
                        printf("      %02X:%04X cycles: AOT %ld vs interp %ld (%+ld)\n",
                               pc24 >> 16, pc24 & 0xffff,
                               acycles, icycles, acycles - icycles);
                    cycle_mismatch++;
                } else cycle_match++;
            }

            if (memcmp(after_interp, g_ram, 0x2000) != 0 ||
                cs.A != iA || cs.X != iX || cs.Y != iY)
                body_bad = 1;
        }
        if (body_skip) { nonterm++; continue; }
        tested++;
        if (body_bad) {
            if (nfail < 8) first_fail[nfail++] = pc24;
            failing_pcs++;
        } else matched++;
    }

    printf("pure-leaf differential: %d bodies tested, 8 randomised trials each\n", tested);
    printf("  identical WRAM + A/X/Y : %d\n", matched);
    printf("  divergent              : %d\n", failing_pcs);
    printf("  no compiled body       : %d\n", skipped);
    printf("  skipped, no clean RTS  : %d\n", nonterm);
    printf("  returns != NORMAL      : %d\n", nonnormal);
    printf("  cycle counts agree     : %d of %d trials\n",
           cycle_match, cycle_match + cycle_mismatch);
    for (int i = 0; i < nfail; i++)
        printf("    diverged: %02X:%04X\n", first_fail[i] >> 16, first_fail[i] & 0xffff);
    return failing_pcs ? 1 : 0;
}
