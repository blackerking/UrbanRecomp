/* SimCity (SNES) stream decompressor -- 00:90dd, decompiled.
 *
 * Why this exists: entering the overview map from the menu freezes for ~140
 * frames, and this routine is 48% of that window (docs/ROM_MAP.md). The other
 * 46% is the software renderer at 02:899b. Nothing here is waiting on
 * hardware -- it is a byte-at-a-time stream decoder running on a 3.58 MHz CPU,
 * so a host implementation removes the cost outright.
 *
 * READ THIS BEFORE CHANGING ANYTHING: the listing this was written from came
 * from tools/dis_cov.py, which decodes each address at the width it actually
 * executed at, recorded by SC_MX_BITMAP. Read at a guessed width the same
 * bytes disassemble into plausible nonsense -- that mistake is what left this
 * routine and 02:8b00 unidentified for most of a session. If you re-check this
 * against the ROM, use the bitmap, not an assumption.
 *
 * The format. One command byte `c`:
 *
 *   c == $FF                      end of stream
 *   (c & $E0) == $E0              long form:  cmd = (c << 3) & $E0
 *                                             len = (((c & 3) << 8) | next) + 1
 *   otherwise                     short form: cmd = c & $E0
 *                                             len = (c & $1F) + 1
 *
 * and then, for `len` bytes:
 *
 *   $00  direct    copy `len` bytes straight from the source
 *   $20  fill      one byte, repeated
 *   $40  fill2     two bytes, alternating
 *   $60  ramp      one byte, incrementing each time
 *   $80  copy16    back-reference; 16-bit offset from the START of the output
 *   $A0  copy16^   same, each byte XOR $FF
 *   $C0  copy8     back-reference; 8-bit offset BACK from the write position
 *   $E0  copy8^    same, each byte XOR $FF
 *
 * The two back-reference forms share one loop in the ROM ($9222), which is why
 * they share one here: $C0 computes its read pointer and branches into $80's
 * body. They copy a byte at a time through the output, so an overlapping run
 * (offset 1, length 40) legitimately repeats what it just wrote -- do not
 * "optimise" that into a memmove.
 *
 * Long form packs the command into bits 4-2 of `c`, which is why it shifts
 * left by three: (c << 3) & $E0 lifts those three bits into the same position
 * the short form's command already occupies, so both feed one dispatch.
 */

#include "simcity_decomp.h"

#include <string.h>

unsigned long g_sc_decomp_cmd_hits[8];
unsigned long g_sc_decomp_bank_wraps;

/* 00:926d -- the source pointer wrapping out of a bank. The ROM sets Y back to
 * $8000 rather than $0000 because the source is LoROM: only the upper half of
 * each bank is mapped, so the next byte after $xxFFFF is $(xx+1)8000. */
static uint8_t sc_decomp_fetch(ScDecompRead rd, void *ctx,
                               uint8_t *bank, uint16_t *y) {
    const uint8_t v = rd(ctx, ((uint32_t)*bank << 16) | *y);
    if (++*y == 0) { *y = 0x8000; ++*bank; g_sc_decomp_bank_wraps++; }
    return v;
}

void sc_decomp_run(uint8_t *wram, ScDecompRead rd, void *ctx,
                   uint8_t src_bank, uint16_t src_y, uint16_t dest_x,
                   ScDecompResult *out) {
    uint8_t  bank = src_bank;
    uint16_t y    = src_y;
    uint16_t x    = dest_x;             /* the guest's X */
    const uint16_t start_x = dest_x;    /* $000e, never advanced by the ROM */
    uint16_t cd = 0, flag = 0;
    uint32_t written = 0;
    int commands = 0, bad = 0;

    /* $7E8000,X with a 16-bit X reaches $7E8000..$7EFFFF and then $7F0000 up,
     * which is a linear 0x8000..0x17FFF in a 128 KB WRAM image -- so the index
     * needs no wrapping of its own. */
#define SC_DST(i) wram[0x8000u + (uint16_t)(i)]

    for (;;) {
        const uint8_t c = sc_decomp_fetch(rd, ctx, &bank, &y);
        if (c == 0xff) break;                       /* 00:9102 */

        unsigned cmd, len;
        if ((c & 0xe0) == 0xe0) {                   /* 00:9109 */
            cmd = (unsigned)(c << 3) & 0xe0;
            const uint8_t lo = sc_decomp_fetch(rd, ctx, &bank, &y);
            len = ((unsigned)(c & 0x03) << 8 | lo) + 1u;
        } else {                                    /* 00:9131 */
            cmd = c & 0xe0;
            len = (unsigned)(c & 0x1f) + 1u;
        }

        g_sc_decomp_cmd_hits[cmd >> 5]++;

        switch (cmd) {
        case 0x00:                                  /* 00:9150 direct */
            for (unsigned i = 0; i < len; i++)
                SC_DST(x++) = sc_decomp_fetch(rd, ctx, &bank, &y);
            break;

        case 0x20: {                                /* 00:916b fill */
            const uint8_t b = sc_decomp_fetch(rd, ctx, &bank, &y);
            for (unsigned i = 0; i < len; i++) SC_DST(x++) = b;
            break;
        }

        case 0x40: {                                /* 00:9187 alternating */
            const uint8_t b0 = sc_decomp_fetch(rd, ctx, &bank, &y);
            const uint8_t b1 = sc_decomp_fetch(rd, ctx, &bank, &y);
            cd = (uint16_t)(b0 | (b1 << 8));
            for (unsigned i = 0; i < len; i++)
                SC_DST(x++) = (i & 1u) ? b1 : b0;
            break;
        }

        case 0x60: {                                /* 00:91c8 ramp */
            uint8_t b = sc_decomp_fetch(rd, ctx, &bank, &y);
            for (unsigned i = 0; i < len; i++) SC_DST(x++) = b++;
            break;
        }

        case 0x80: case 0xa0:                       /* 00:91e5 copy, 16-bit */
        case 0xc0: case 0xe0: {                     /* 00:9245 copy, 8-bit */
            const int invert = (cmd & 0x20) != 0;
            uint16_t rp;
            flag = (uint16_t)(cmd & 0x20);          /* $0010, $0011 stays 0 */
            if (cmd < 0xc0) {
                const uint8_t lo = sc_decomp_fetch(rd, ctx, &bank, &y);
                const uint8_t hi = sc_decomp_fetch(rd, ctx, &bank, &y);
                /* 00:9216 -- the offset is relative to where this call STARTED
                 * writing ($000e), not to the current position. */
                rp = (uint16_t)((uint16_t)(lo | (hi << 8)) + start_x);
            } else {
                const uint8_t off = sc_decomp_fetch(rd, ctx, &bank, &y);
                rp = (uint16_t)(x - off);           /* 00:9263 TXA/SEC/SBC */
            }
            cd = rp;
            for (unsigned i = 0; i < len; i++) {
                const uint8_t v = SC_DST(rp++);
                SC_DST(x++) = invert ? (uint8_t)(v ^ 0xff) : v;
            }
            cd = rp;
            break;
        }

        default:                                    /* unreachable: cmd is a
                                                     * 3-bit field, all eight
                                                     * values are handled */
            bad = 1;
            break;
        }

        written += len;
        commands++;
        if (bad) break;

        /* The ROM has no bound here -- it trusts the stream and stops on $FF.
         * A corrupt or misaimed source would spin forever, and unlike the guest
         * we are inside the host's frame loop with no way out, so cap it. The
         * cap is far above anything real: the whole output region is 64 KB. */
        if (written > 0x20000u || commands > 0x20000) { bad = 1; break; }
    }

#undef SC_DST

    if (out) {
        out->src_y    = y;
        out->src_bank = bank;
        out->x        = x;
        out->cd       = cd;
        out->flag     = flag;
        out->bytes_out = written;
        out->commands = commands;
        out->bad      = bad;
    }
}
