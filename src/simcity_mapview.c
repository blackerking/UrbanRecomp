/* Host-side map renderer. Port of tools/render_map.py, which was validated
 * against real play before any of this was written -- the Python render and the
 * guest were confirmed to show the same area, and the host output was actually
 * *cleaner* than the guest's after a save-state load (docs/OPEN_QUESTIONS.md
 * E1). Keeping the Python around means this C can be diffed against a known-good
 * implementation rather than only against itself.
 *
 * Chain, from docs/REFERENCE_map_format.md:
 *
 *   v  = cell & 0x03FF                 10-bit tile index
 *   tp = u16(TILE_ADDR + v*2)          background tile
 *   tu = u16(TILU_ADDR + v*2)          overlay tile, 0x300 = empty, drawn -1,-1
 *   CHR: 4bpp SNES, tc & 0x3FF = tile, (tc >> 10) & 7 = palette
 *   palette: BGR555 at $7E2440
 */
#include <string.h>

#include "snes/snes.h"
#include "snes/ppu.h"
#include "simcity_mapview.h"
#include <stdio.h>

/* ARRAY, not a pointer -- debug_server.h has `extern uint8_t g_ram[]`.
 * Declaring it `uint8_t *g_ram` here linked without complaint (C externs carry
 * no type to the linker) and then read the first 8 bytes of WRAM AS a pointer
 * and dereferenced them. Instant segfault, and no build diagnostic anywhere.
 * Same family as the SDL float-vs-int pointer mismatch: it compiles, it links, it is
 * wrong. */
extern uint8_t g_ram[];     /* 128K WRAM: $7E0000 at 0, $7F0000 at 0x10000 */
extern Ppu     *g_ppu;
extern Snes    *g_snes;

/* WRAM offsets */
#define SC_MAP_OFF   0x010200u   /* $7F0200 */
#define SC_PAL_OFF   0x002440u   /* $7E2440 */
#define SC_SCROLL_X  0x0001bdu
#define SC_SCROLL_Y  0x0001bfu

/* ROM file offsets. USA image -- docs/REFERENCE_map_format.md notes the table
 * address is region-specific, and docs/REGIONS.md measured that E/F/G/J are a
 * different build entirely, so this is gated rather than assumed portable. */
#define SC_TILE_ADDR 0x0156A9u
#define SC_TILU_ADDR (SC_TILE_ADDR - 0x77Cu)

#define SC_MAP_W 120
#define SC_MAP_H 100

static bool s_rom_is_us;

void ScMapView_SetRomIsUs(bool is_us) { s_rom_is_us = is_us; }

static uint16_t ram_u16(uint32_t off) {
    return (uint16_t)(g_ram[off] | (g_ram[off + 1] << 8));
}

void ScMapView_GetScroll(int *sx, int *sy) {
    if (sx) *sx = (int8_t)g_ram[SC_SCROLL_X];
    if (sy) *sy = (int8_t)g_ram[SC_SCROLL_Y];
}

/* BGR555 -> ARGB8888. */
static uint32_t pal_entry(unsigned index) {
    uint16_t v = ram_u16(SC_PAL_OFF + (index & 0xFFu) * 2u);
    uint32_t r = (uint32_t)((v & 31u) * 255u / 31u);
    uint32_t g = (uint32_t)(((v >> 5) & 31u) * 255u / 31u);
    uint32_t b = (uint32_t)(((v >> 10) & 31u) * 255u / 31u);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* One 4bpp SNES tile -> 64 palette indices. VRAM is uint16 words; a tile is 32
 * bytes = 16 words, bitplanes 0/1 then 2/3. */
static void decode_tile(unsigned tile, uint8_t out[64]) {
    const uint16_t *vram = g_ppu->vram;
    unsigned base = (tile & 0x3FFu) * 16u;      /* in words */
    if (base + 16u > 0x8000u) { memset(out, 0, 64); return; }
    for (int y = 0; y < 8; y++) {
        uint16_t lo = vram[base + (unsigned)y];        /* planes 0,1 */
        uint16_t hi = vram[base + 8u + (unsigned)y];   /* planes 2,3 */
        uint8_t p0 = (uint8_t)(lo & 0xFF), p1 = (uint8_t)(lo >> 8);
        uint8_t p2 = (uint8_t)(hi & 0xFF), p3 = (uint8_t)(hi >> 8);
        for (int x = 0; x < 8; x++) {
            int bit = 7 - x;
            out[y * 8 + x] = (uint8_t)(((p0 >> bit) & 1)
                                     | (((p1 >> bit) & 1) << 1)
                                     | (((p2 >> bit) & 1) << 2)
                                     | (((p3 >> bit) & 1) << 3));
        }
    }
}

static void blit_tile(uint8_t *out, int pitch, int w, int h,
                      unsigned tc, int px, int py) {
    uint8_t pix[64];
    decode_tile(tc, pix);
    unsigned pbase = ((tc >> 10) & 7u) * 16u;
    for (int ty = 0; ty < 8; ty++) {
        int oy = py + ty;
        if (oy < 0 || oy >= h) continue;
        uint32_t *row = (uint32_t *)(out + (size_t)oy * pitch);
        for (int tx = 0; tx < 8; tx++) {
            int ox = px + tx;
            if (ox < 0 || ox >= w) continue;
            uint8_t ci = pix[ty * 8 + tx];
            if (ci == 0) continue;              /* transparent */
            row[ox] = pal_entry(pbase + ci);
        }
    }
}

bool ScMapView_Render(uint8_t *out, int pitch, int cols, int rows,
                      int sx, int sy) {
    if (!out || !g_ppu || !g_snes || !g_snes->cart ||
        !g_snes->cart->rom || !s_rom_is_us) return false;

    const uint8_t *rom = g_snes->cart->rom;
    const int w = cols * 8, h = rows * 8;

    for (int y = 0; y < h; y++)
        memset(out + (size_t)y * pitch, 0, (size_t)w * 4);

    /* Backgrounds first, then overlays at -1,-1 so they overlap up and left. */
    for (int pass = 0; pass < 2; pass++) {
        for (int ry = 0; ry < rows; ry++) {
            int my = sy + ry;
            if (my < 0 || my >= SC_MAP_H) continue;
            for (int rx = 0; rx < cols; rx++) {
                int mx = sx + rx;
                if (mx < 0 || mx >= SC_MAP_W) continue;
                unsigned v = ram_u16(SC_MAP_OFF +
                                     (uint32_t)((my * SC_MAP_W + mx) * 2)) & 0x03FFu;
                if (pass == 0) {
                    unsigned tp = (unsigned)(rom[SC_TILE_ADDR + v * 2] |
                                             (rom[SC_TILE_ADDR + v * 2 + 1] << 8));
                    blit_tile(out, pitch, w, h, tp, rx * 8, ry * 8);
                } else {
                    unsigned tu = (unsigned)(rom[SC_TILU_ADDR + v * 2] |
                                             (rom[SC_TILU_ADDR + v * 2 + 1] << 8));
                    if ((tu & 0x3FFu) == 0x300u) continue;
                    blit_tile(out, pitch, w, h, tu, rx * 8 - 1, ry * 8 - 1);
                }
            }
        }
    }
    return true;
}
