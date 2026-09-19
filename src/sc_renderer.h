#pragma once
#include "sc_video.h"
#include <stdint.h>
#include <stddef.h>
typedef struct Ppu Ppu;
typedef struct ScRenderer {
    ScViewport view;
    uint32_t *pixels;
    size_t capacity;
    const uint8_t *rom;
    size_t rom_size;
    bool rom_is_us;
    int wood_layer, wood_period;
    uint16_t wood_rows[32];
    bool title_live;
    bool city_frame;
} ScRenderer;
void ScRendererInit(ScRenderer *r, const uint8_t *rom, size_t size, bool is_us);
bool ScRendererResize(ScRenderer *r, ScViewport view);
void ScRendererDestroy(ScRenderer *r);
/* Called AFTER the stock PPU has drawn a line, BEFORE the guest advances.
 * Reads only: no guest writes, PPU replay, state forcing, or hidden frames. */
void ScRendererLine(ScRenderer *r, const Ppu *ppu, const uint8_t *ram,
                    int line, const uint32_t *native);
/* Public for ROM-free edge/flip/bounds tests and captured-frame oracles. */
uint32_t ScRendererMapPixel(const ScRenderer *r, const Ppu *ppu,
                            const uint8_t *ram, int world_x, int world_y);
