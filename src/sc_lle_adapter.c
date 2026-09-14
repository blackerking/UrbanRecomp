/* The device layer charges MDMA through CpuState. This interpreter host owns
 * its beam/APU loop, so defer that time to the same loop as opcode time.
 * Keep the ABI object separate from main.c's private Interp816 pointer. */
#include "cpu_state.h"
#include "snes/snes.h"
#include "sc_lle_adapter.h"

CpuState g_cpu;
extern Snes *g_snes;

void ScLleWrite(uint32_t address, uint8_t value) {
    snes_beam_hold(1);
    snes_write(g_snes, address, value);
    snes_beam_hold(0);
}

uint64_t ScLleTakeDmaCycles(void) {
    uint64_t cycles = g_cpu.master_cycles;
    g_cpu.master_cycles = 0;
    return cycles;
}

/* No presentation mod observes or replaces APU reads in this LLE host. */
uint8_t rtl_apu_port_observers_read(uint16_t reg, uint8_t value) {
    (void)reg;
    return value;
}
