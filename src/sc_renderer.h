#pragma once
#include "sc_video.h"
#include "sc_selector.h"
#include "sc_vehicles.h"
#include <stdint.h>
#include <stddef.h>
typedef struct Ppu Ppu;
typedef struct ScRenderer {
    ScViewport view;
    ScViewport gameplay_view; /* configured HUD anchor; menus are centered */
    uint32_t *pixels;
    uint32_t *advisor_pixels;
    bool advisor_frame;
    size_t capacity;
    const uint8_t *rom;
    size_t rom_size;
    bool rom_is_us;
    int wood_layer, wood_period;
    uint16_t wood_rows[32];
    bool title_live;
    bool city_frame;
    int light_slot, light_x, light_pitch;
    bool scroll_valid;
    int scroll_x, scroll_y, scroll_h, scroll_v, scroll_adjust_x, scroll_adjust_y, scroll_still;
    bool objects_valid;
    int16_t object_raw[128], object_x[128];
    uint16_t object_attr[128];
    uint8_t object_y[128], object_grace[128];
    Ppu *held_ppu;
    uint8_t held_map[24000], previous_map[24000];
    bool map_valid, map_hold, map_confirmed, map_dark;
    int map_quiet, map_age, held_x, held_y;
    uint8_t repaired_edges[224]; /* per row: bit 0 left 8 px, bit 1 right */
    bool sylt;                   /* the ninth scenario card is on */
    /* The vehicles kept for the margin (src/sc_vehicles.c), handed in by the
     * host before each frame's first line. */
    ScVehicleSprite vehicles[19];
    int vehicle_count;
    ScSelSprite selector[SC_SEL_MAX_SPRITES]; /* pins and marks, this frame */
    int selector_count;
    const uint16_t *selector_row; /* render_row's current row, for scenery */
} ScRenderer;
void ScRendererInit(ScRenderer *r, const uint8_t *rom, size_t size, bool is_us);
bool ScRendererResize(ScRenderer *r, ScViewport view);
void ScRendererDestroy(ScRenderer *r);
void ScRendererResetHistory(ScRenderer *r);
/* Called AFTER the stock PPU has drawn a line, BEFORE the guest advances.
 * Reads only: no guest writes, PPU replay, state forcing, or hidden frames. */
void ScRendererLine(ScRenderer *r, const Ppu *ppu, const uint8_t *ram,
                    int line, const uint32_t *native);
/* Public for ROM-free edge/flip/bounds tests and captured-frame oracles. */
uint32_t ScRendererMapPixel(const ScRenderer *r, const Ppu *ppu,
                            const uint8_t *ram, int world_x, int world_y);
