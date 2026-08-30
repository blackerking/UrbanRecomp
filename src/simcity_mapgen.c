/* SimCity (SNES) map generation, decompiled to native C.
 *
 * GOAL: generate maps without running guest code, so generation can be
 * changed -- bigger maps, new terrain rules, custom seeds -- rather than only
 * replayed. That means every routine here must reproduce the ROM bit-exactly
 * first; only once a seed yields an identical map does deviating from it mean
 * anything.
 *
 * STATUS: the PRNG and the seeding are done and are transcribed from the
 * disassembly below. The terrain feature routines are NOT started. Nothing
 * here is wired into the host yet, and nothing has been verified against the
 * guest -- see "Verification" at the bottom, which is the next piece of work
 * and the one that makes the rest trustworthy.
 *
 * Map geometry: 120 x 100 = 12000 cells. Confirmed independently by the power
 * bitmap at 03:b0f8, whose CPX #$05dc bounds it at 1500 bytes = 12000 bits.
 *
 * The generated map lands at $7E0200 and is later masked with AND #$03FF into
 * $7E8000 by the copy loop at 03:cf82-cf9d, which strips flag bits from each
 * tile. See docs/ROM_MAP.md. */

#include "simcity_mapgen.h"

/* ── PRNG ──────────────────────────────────────────────────────────────────
 *
 * 00:824f, reached through the JSL wrapper at 00:824b:
 *
 *     REP #$20        ; 16-bit A
 *     CLC
 *     LDA $59
 *     STA $5d         ; t   = s0
 *     ADC $5b         ; A   = s0 + s1, carry clear on entry
 *     STA $59         ; s0' = s0 + s1
 *     ADC $5d         ; A   = s0' + t + CARRY from the previous ADC
 *     STA $5b         ; s1' = that
 *     RTS             ; returns s1' in A
 *
 * An additive lagged-Fibonacci-with-carry generator over two 16-bit words. It
 * takes no input, which is what rules out the "checksum/hash" reading an
 * earlier pass of docs/ROM_MAP.md recorded -- it folds nothing in, it only
 * advances state.
 *
 * The carry chaining between the two ADCs is load-bearing: the second one adds
 * the carry out of the first. Dropping it gives a stream that looks plausible
 * and diverges within a few draws. */
void sc_mapgen_prng_seed_from_spin(ScMapGenPrng *p, uint16_t spin_counter) {
    /* 00:823e: LDA $c7 / STA $59 / INC A / STA $5b / INC A / STA $5d.
     * $c7 counts vblank spin iterations, so the seed is how long the player
     * took -- which is what makes generated maps vary at all. */
    p->s0 = spin_counter;
    p->s1 = (uint16_t)(spin_counter + 1u);
    p->t  = (uint16_t)(spin_counter + 2u);
}

uint16_t sc_mapgen_prng_step(ScMapGenPrng *p) {
    const uint16_t t = p->s0;
    p->t = t;

    const uint32_t a = (uint32_t)p->s0 + (uint32_t)p->s1;   /* CLC: no carry in */
    const unsigned carry = (a >> 16) & 1u;
    p->s0 = (uint16_t)a;

    const uint32_t b = (uint32_t)p->s0 + (uint32_t)t + carry;
    p->s1 = (uint16_t)b;

    return p->s1;
}

/* ── Map seeding ───────────────────────────────────────────────────────────
 *
 * 03:d840. Entry has the caller's value in A, which it stores to $59:
 *
 *     STA $59
 *     LDA $0b28 / EOR #$ffff / ROL A x5 / ADC #$1238 / STA $5b
 *     STZ $5d
 *     LDA $0b29 / ASL A / ADC $0b28 / ADC $0b27 / AND #$001f / TAX
 *     JSL $00824b / DEX / BPL      ; steps the PRNG X+1 times, 1..32
 *     JSL $01f1ed                  ; terrain features
 *     JSL $02923f                  ; scratch clear + DMA upload
 *
 * The map is fully determined by the three seed bytes $0b27-$0b29: 03:d873
 * copies them to $0b2a-$0b2c straight afterwards, i.e. the game keeps them as
 * the map's identity.
 *
 * CARRY IS UNDEFINED ON ENTRY here, and both the ROL chain and the ADC #$1238
 * consume it. The ROLs rotate it through bit 0 and the ADC adds it. So the
 * result depends on the caller's carry, which the disassembly alone cannot
 * tell us -- this is the first thing the verification harness has to pin down,
 * and it is why `entry_carry` is a parameter rather than an assumption. */
void sc_mapgen_seed(ScMapGenPrng *p, uint16_t a_on_entry,
                    uint8_t seed0, uint8_t seed1, uint8_t seed2,
                    unsigned entry_carry) {
    p->s0 = a_on_entry;                       /* STA $59 */

    uint32_t v = (uint16_t)~((uint16_t)seed1);    /* LDA $0b28 / EOR #$ffff */
    unsigned c = entry_carry & 1u;
    for (int i = 0; i < 5; i++) {                 /* ROL A x5, through carry */
        const unsigned out = (v >> 15) & 1u;
        v = (uint16_t)((v << 1) | c);
        c = out;
    }
    v = v + 0x1238u + c;                          /* ADC #$1238, carry in */
    p->s1 = (uint16_t)v;

    p->t = 0;                                     /* STZ $5d */

    /* LDA $0b29 / ASL A / ADC $0b28 / ADC $0b27 / AND #$001f.
     * ASL clears carry into the first ADC only if bit 15 was 0; seed2 is a
     * byte here, so the high byte is whatever $0b29's word read gives -- the
     * harness has to confirm this is a byte read, not a word read. */
    uint32_t n = (uint32_t)seed2 << 1;
    unsigned c2 = (n >> 16) & 1u;
    n = (uint16_t)n;
    n = n + seed1 + c2;  c2 = (n >> 16) & 1u;  n = (uint16_t)n;
    n = n + seed0 + c2;                        n = (uint16_t)n;
    const unsigned steps = (unsigned)(n & 0x1fu) + 1u;   /* DEX/BPL: X+1 times */

    for (unsigned i = 0; i < steps; i++) sc_mapgen_prng_step(p);
}

/* ── Not yet decompiled ────────────────────────────────────────────────────
 *
 * 01:f1ed  dispatcher. Draws a byte from the PRNG and branches: roughly a
 *          third of the time to $f22c, otherwise through a chain of five
 *          feature routines. This is the evidence that generation is genuinely
 *          procedural rather than a table of prebuilt maps.
 *
 *          01:f22c   86 instructions
 *          01:f380   14
 *          01:f5b9   24
 *          01:f311   45
 *          01:f444   65
 *          01:f3a3   18
 *
 * 02:923f  zero-fills $7EA400-$7EBFFF (7168 bytes), then sets up the DMA. The
 *          upload side, not generation proper -- a native generator writes the
 *          cells directly and does not need it.
 *
 * ── Verification, which comes before any of that ───────────────────────────
 *
 * None of this is worth anything until a seed produces an identical map. The
 * harness to build first:
 *
 *   1. Trace the guest. The coverage hook `g_interp_bridge_pc_hook` already
 *      fires per interpreted opcode, so watching for PC == $00824f and
 *      recording $59/$5b/$5d gives the reference PRNG stream, and settles the
 *      entry-carry question above by observation rather than assumption.
 *   2. Capture the map. Dump $7E0200 (12000 cells) after generation with the
 *      seed bytes $0b27-$0b29 alongside; that pair is the golden reference.
 *   3. Compare per routine, not just at the end. A whole-map mismatch says
 *      nothing about WHICH of six routines is wrong.
 *
 * Two traps this project has already hit that apply directly here: use
 * tools/dis_mx.py and not dis65816.py, because the latter does not track
 * SEP/REP and will mis-size operands after a width change; and do not trust a
 * harness-side number as if it came from the emulator -- the audio work
 * measured the test harness twice before noticing. */
