/* The game's stream decompressor, decompiled. See sc_decomp.c. */
#ifndef SC_DECOMP_H_INCLUDED
#define SC_DECOMP_H_INCLUDED

#include <stdint.h>

/* Reads one byte of the compressed source. The source lives in ROM, so this
 * cannot be a plain WRAM pointer the way the map generator's state was. */
typedef uint8_t (*ScDecompRead)(void *ctx, uint32_t addr24);

/* Guest state the routine leaves behind, so a caller can write it back and be
 * indistinguishable from having run 00:90dd. */
typedef struct ScDecompResult {
    uint16_t src_y;      /* $0009  -- source offset, just past the last byte */
    uint8_t  src_bank;   /* the bank reads ended in (the guest leaves $000b
                          * ALONE -- 00:926d bumps DB, not the variable) */
    uint16_t x;          /* final destination index */
    uint16_t cd;         /* $000c/$000d scratch pair */
    uint16_t flag;       /* $0010/$0011 -- last long-copy's invert flag */
    uint32_t bytes_out;  /* how much was written (diagnostic) */
    int      commands;   /* how many command bytes were consumed */
    int      bad;        /* non-zero if the stream ran away -- see the .c */
} ScDecompResult;

/* 00:90dd. Writes into `wram` at 0x8000 + X, which is $7E8000,X. */
/* Per-command hit counts (indexed by cmd >> 5) and source bank crossings,
 * so a clean verify run can be read as "which paths were actually tested".
 * A path with a zero here is UNVERIFIED, however green the totals look. */
extern unsigned long g_sc_decomp_cmd_hits[8];
extern unsigned long g_sc_decomp_bank_wraps;

void sc_decomp_run(uint8_t *wram, ScDecompRead rd, void *ctx,
                   uint8_t src_bank, uint16_t src_y, uint16_t dest_x,
                   ScDecompResult *out);

#endif
