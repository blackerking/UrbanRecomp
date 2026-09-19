/* Fiber-mode guest driver -- migration step 3d. See src/sc_fiberdrive.c.
 *
 * Kept behind this narrow header because the implementation needs
 * cpu_state.h, whose global `CpuState g_cpu` collides with src/main.c's
 * `Interp816 *g_cpu`. Nothing here exposes either type. */
#ifndef SC_FIBERDRIVE_H_INCLUDED
#define SC_FIBERDRIVE_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

/* Creates the game fiber with the compiled reset handler as its entry and
 * installs the vblank yield. Returns true on success. Idempotent. */
bool ScFiberDrive_Init(void);
/* Re-point the fiber's CpuState at a just-loaded save state. Without this
 * the fiber keeps Init()'s reset registers over restored WRAM. */
struct Interp816;
void ScFiberDrive_AdoptInterpState(const struct Interp816 *in);

/* Switches into the guest and runs until it yields at the vblank wait.
 * Returns false if the guest stopped making progress -- the reset handler
 * returned, or too many frames passed with no yield. `frame` is only used
 * for diagnostics. */
/* `nmi_pending` is the host's per-frame NMI request, consumed from the
 * interpreter CPU by the caller. When set, the driver materializes the
 * 65816 interrupt frame and resumes at the NMI vector, so the guest runs
 * 00:80B2 itself instead of the driver faking its INC $b9. */
bool ScFiberDrive_RunGuestSlice(uint64_t frame, bool nmi_pending,
                                     uint64_t budget);

/* NMIs actually delivered to the guest (for the --qualify nmi_serviced). */
uint64_t ScFiberDrive_NmiDelivered(void);

uint64_t ScFiberDrive_MasterCycles(void);

#endif /* SC_FIBERDRIVE_H_INCLUDED */
