/* Fiber-mode guest driver -- migration step 3d. See src/simcity_fiberdrive.c.
 *
 * Kept behind this narrow header because the implementation needs
 * cpu_state.h, whose global `CpuState g_cpu` collides with src/main.c's
 * `Interp816 *g_cpu`. Nothing here exposes either type. */
#ifndef SIMCITY_FIBERDRIVE_H
#define SIMCITY_FIBERDRIVE_H

#include <stdbool.h>
#include <stdint.h>

/* Creates the game fiber with the compiled reset handler as its entry and
 * installs the vblank yield. Returns true on success. Idempotent. */
bool SimCityFiberDrive_Init(void);

/* Switches into the guest and runs until it yields at the vblank wait.
 * Returns false if the guest stopped making progress -- the reset handler
 * returned, or too many frames passed with no yield. `frame` is only used
 * for diagnostics. */
bool SimCityFiberDrive_RunGuestFrame(uint64_t frame);

uint64_t SimCityFiberDrive_MasterCycles(void);

#endif /* SIMCITY_FIBERDRIVE_H */
