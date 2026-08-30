/* SimCity (SNES) map generation, decompiled. See simcity_mapgen.c. */
#ifndef SIMCITY_MAPGEN_H
#define SIMCITY_MAPGEN_H

#include <stdint.h>

/* The map is 120 x 100 cells. Confirmed by the power bitmap at 03:b0f8, whose
 * CPX #$05dc bounds it at 1500 bytes = 12000 bits, one per cell. */
enum { SC_MAPGEN_W = 120, SC_MAPGEN_H = 100, SC_MAPGEN_CELLS = 12000 };

/* $59 / $5b / $5d -- two 16-bit state words plus the temp the step writes. */
typedef struct ScMapGenPrng {
    uint16_t s0;   /* $59 */
    uint16_t s1;   /* $5b */
    uint16_t t;    /* $5d */
} ScMapGenPrng;

/* 00:823e -- seed from the vblank spin counter $c7. */
void sc_mapgen_prng_seed_from_spin(ScMapGenPrng *p, uint16_t spin_counter);

/* 00:824f -- one step; returns the new $5b, as the ROM returns it in A. */
uint16_t sc_mapgen_prng_step(ScMapGenPrng *p);

/* 03:d840 -- fold the three map-seed bytes in and pre-step the PRNG 1..32
 * times. `entry_carry` is a parameter and not an assumption: the ROL chain and
 * the ADC both consume the caller's carry, which the disassembly cannot tell
 * us. The verification harness settles it. */
void sc_mapgen_seed(ScMapGenPrng *p, uint16_t a_on_entry,
                    uint8_t seed0, uint8_t seed1, uint8_t seed2,
                    unsigned entry_carry);

#endif
