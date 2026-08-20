/* Host-side SimCity map renderer -- see docs/PLAN_renderer.md Stage 2.
 *
 * Draws the live 120x100 city map from WRAM using tile graphics out of VRAM,
 * entirely host-side. The guest still owns every bit of state; this only draws
 * it differently, which is the state/presentation line the plan sets out.
 *
 * Kept in its own translation unit so main.c stays free of the tile-decode
 * detail, the same reason simcity_fiberdrive.c is separate. */
#ifndef SIMCITY_MAPVIEW_H
#define SIMCITY_MAPVIEW_H

#include <stdbool.h>
#include <stdint.h>

/* True when the loaded ROM is the one the tile tables were mapped against. */
void ScMapView_SetRomIsUs(bool is_us);

/* Render `cols` x `rows` map cells starting at cell (sx, sy) into `out` as
 * ARGB8888 with `pitch` bytes per row. Returns false if the ROM is not the US
 * image (the tile-table addresses are US-specific) or inputs are missing. */
bool ScMapView_Render(uint8_t *out, int pitch, int cols, int rows,
                      int sx, int sy);

/* Current map scroll in cells, read from $01bd/$01bf. */
void ScMapView_GetScroll(int *sx, int *sy);

#endif /* SIMCITY_MAPVIEW_H */
