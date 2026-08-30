/* SimCity (SNES) map generation, decompiled to native C.
 *
 * GOAL: generate maps without running guest code, so generation can be
 * changed -- bigger maps, new terrain rules, custom seeds -- rather than only
 * replayed. That means every routine here must reproduce the ROM bit-exactly
 * first; only once a seed yields an identical map does deviating from it mean
 * anything.
 *
 * STATUS: the PRNG is decompiled and VERIFIED against the running guest --
 * 14 of 14 sampled state transitions reproduced exactly, one step apart. The
 * seeding is decompiled but NOT verified (its entry carry is still unknown).
 * The terrain feature routines are not started.
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
 * the carry out of the first. Measured against the guest, dropping it drops
 * the match from 14 of 14 sampled transitions to 8 of 14 -- so it is wrong,
 * and it is wrong in the worst way, agreeing most of the time. */
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

/* ── Random below N ────────────────────────────────────────────────────────
 *
 * 01:f877. The generator's range primitive, and the reason the PRNG had to be
 * exact before anything else could be:
 *
 *     REP #$20
 *     INC A / STA $79        ; keep N+1
 *     JSL $00824b            ; one PRNG step, result in A (16-bit)
 *     SEP #$20 / XBA         ; take the HIGH byte of that result
 *     LDA $79 / PHA
 *     LDA $b3 / AND #$7f / STA $b1      ; unrelated: $b1/$b3 housekeeping
 *     PLA  / STA $4202       ; WRMPYA = (N+1) low byte
 *     XBA  / STA $4203       ; WRMPYB = the random byte -> starts the multiply
 *     PHA / PLA / NOP        ; the mandatory 8-cycle wait
 *     LDA $4217 / XBA / LDA $4216 / PHA
 *     ...
 *     PLA / XBA / REP #$20 / AND #$00ff / RTS
 *
 * It runs the SNES hardware multiplier over (N+1) x rand8 and returns the HIGH
 * byte of the 16-bit product -- the standard trick for scaling a byte into
 * 0..N without a divide. The final `AND #$00ff` clears the high half, so the
 * result really is 0..N inclusive.
 *
 * The `$b1`/`$b3` traffic in the middle is not part of the computation; it is
 * preserved here only because reproducing the PRNG consumption exactly is what
 * matters, and that is one step per call. */
uint16_t sc_mapgen_rand_below(ScMapGenPrng *p, uint16_t n) {
    const uint16_t r = sc_mapgen_prng_step(p);       /* JSL $00824b */
    const unsigned rand8 = (r >> 8) & 0xffu;         /* XBA: the high byte */
    const unsigned mul = (unsigned)((n + 1u) & 0xffu) * rand8;   /* 8x8 -> 16 */
    return (uint16_t)((mul >> 8) & 0xffu);           /* RDMPYH, then AND #$00ff */
}

/* ── Feature: centre point ─────────────────────────────────────────────────
 *
 * 01:f380, the smallest of the six:
 *
 *     LDA #$0028 / JSR $f877 / CLC / ADC #$0028   ; 40 + rand(0..40)
 *     STA $0457 / STA $043b
 *     LDA #$0021 / JSR $f877 / CLC / ADC #$0021   ; 33 + rand(0..33)
 *     STA $0459 / STA $043d
 *
 * On a 120 x 100 map that is x in 40..80 and y in 33..66 -- a point in the
 * middle third of each axis, written to two pairs of variables at once.
 *
 * NOTE the operand order: the range call happens BEFORE the add, so it draws
 * with N = 40 and N = 33, not with the sum. Two PRNG steps per call. */
void sc_mapgen_feature_centre(ScMapGenPrng *p, ScMapGenState *st) {
    st->x0 = (uint16_t)(sc_mapgen_rand_below(p, 0x0028) + 0x0028u);   /* $0457/$043b */
    st->y0 = (uint16_t)(sc_mapgen_rand_below(p, 0x0021) + 0x0021u);   /* $0459/$043d */
}

/* ── Feature: scatter ──────────────────────────────────────────────────────
 *
 * 01:f3a3:
 *
 *     LDA #$0064 / JSR $f877 / CLC / ADC #$0032   ; count = 50 + rand(0..100)
 *     STA $043f
 *   loop:
 *     LDA #$0077 / JSR $f877 / STA $044b          ; x = rand(0..119)
 *     LDA #$0063 / JSR $f877 / STA $044d          ; y = rand(0..99)
 *     JSR $f3d3                                   ; place at (x,y)
 *     DEC $043f / BNE loop
 *     JSR $f502 / JSR $f502
 *
 * 50..150 placements at cells drawn across the FULL map -- 0..119 by 0..99 is
 * exactly the 120 x 100 bounds, which is a useful confirmation of the geometry
 * from a second direction.
 *
 * Three PRNG steps per iteration (one for the count, then two per placement),
 * so the stream position depends on the count drawn first. Getting that order
 * wrong desynchronises everything after it. */
void sc_mapgen_feature_scatter(ScMapGenPrng *p, ScMapGenState *st) {
    uint16_t count = (uint16_t)(sc_mapgen_rand_below(p, 0x0064) + 0x0032u);
    st->count = count;
    while (count) {
        st->px = sc_mapgen_rand_below(p, 0x0077);   /* $044b, 0..119 */
        st->py = sc_mapgen_rand_below(p, 0x0063);   /* $044d, 0..99  */
        /* JSR $f3d3 -- the placement itself, not yet decompiled. */
        count--;
    }
    st->count = 0;
    /* JSR $f502 twice -- not yet decompiled. */
}

/* ── Feature: path through the centre ──────────────────────────────────────
 *
 * 01:f5b9:
 *
 *     JSL $00824b / AND #$0003 / STA $045f / STA $0461   ; dir = rand & 3
 *     JSR $f600                                          ; walk that way
 *     LDA $0457 / STA $043b / LDA $0459 / STA $043d      ; back to the centre
 *     LDA $045f / EOR #$0004 / STA $045f / STA $0461     ; dir ^= 4
 *     JSR $f600                                          ; walk the other way
 *     LDA $0457 / STA $043b / LDA $0459 / STA $043d      ; back to the centre
 *
 * Draws from the centre point in one of four directions, then from the same
 * point in the opposite one -- so the feature crosses the middle rather than
 * starting there. `EOR #$0004` is what makes 0..3 and 4..7 opposite halves of
 * an eight-way direction encoding.
 *
 * Note it steps the PRNG DIRECTLY rather than through 01:f877, so this call
 * costs exactly one step regardless of the direction drawn.
 *
 * $0457/$0459 are the centre written by 01:f380, so this feature depends on
 * that one having run. */
void sc_mapgen_feature_path(ScMapGenPrng *p, ScMapGenState *st) {
    st->dir = (uint16_t)(sc_mapgen_prng_step(p) & 0x0003u);
    /* JSR $f600 -- the walk itself, not yet decompiled. */
    st->cur_x = st->x0;
    st->cur_y = st->y0;
    st->dir ^= 0x0004u;
    /* JSR $f600 again. */
    st->cur_x = st->x0;
    st->cur_y = st->y0;
}

/* ── Not yet decompiled ────────────────────────────────────────────────────
 *
 * 01:f1ed  dispatcher. Draws a byte from the PRNG and branches: roughly a
 *          third of the time to $f22c, otherwise through a chain of five
 *          feature routines. This is the evidence that generation is genuinely
 *          procedural rather than a table of prebuilt maps.
 *
 *          01:f380   DONE (sc_mapgen_feature_centre)
 *          01:f877   DONE (sc_mapgen_rand_below)
 *
 *          01:f3a3   DONE (scatter), except its $f3d3 placement and $f502
 *          01:f5b9   DONE (path),    except its $f600 walk
 *
 *          01:f22c   86 instructions
 *          01:f311   45
 *          01:f444   65
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
 *   1. DONE for the PRNG, by SAMPLING rather than tracing. There is no
 *      per-opcode hook in this target: the interp816 core never calls
 *      interp816_opcode_hook, and interp_bridge.c -- which owns
 *      g_interp_bridge_pc_hook -- is not compiled into it. Both were wired up
 *      and produced no output whatever.
 *
 *      What works instead: SC_MAPGEN_VERIFY=1 logs $59/$5b once per frame, and
 *      because the state is only 32 bits, a correct step must join consecutive
 *      samples in a few iterations while a wrong one essentially never does.
 *      That is the check above, and it discriminates: 14/14 against 8/14.
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
