#!/usr/bin/env python3
"""Select AOT bodies that can be differentially tested from synthetic state.

The differential harness (src/aot_diff.c) seeds registers and WRAM itself
rather than arriving through a real caller. For most routines that is not a
fair test: real callers establish invariants (a particular DB, a direct page,
X/Y already in range) that synthetic state violates, so a divergence would say
nothing about the codegen.

So restrict to routines where synthetic entry state IS legitimate -- pure leaf
routines:

  * no calls or non-local transfers (JSR/JSL/JMP/JML/RTI/BRK/COP), so the body
    is self-contained and its extent is exactly what runs;
  * no absolute access to the hardware register window, so running it twice
    has no device side effects and WRAM is the whole observable result;
  * no stack or frame manipulation -- see the STACKY note below;
  * a real extent from the manifest, and small enough to step under a guard.

Emits a C header with the surviving (pc24, end) pairs. Addresses only -- the
same kind of ROM-derived fact the recomp/*.cfg files already carry.

    python tools/select_pure_leaves.py > src/pure_leaves.h
"""
import json
import sys

MANIFEST = 'src/gen/program_manifest.json'
ROM = 'simcity.sfc'

# Opcodes that leave the routine or call out of it.
TRANSFER = {
    0x20, 0x22, 0xFC,        # JSR abs, JSL long, JSR (abs,X)
    0x4C, 0x5C, 0x6C, 0x7C, 0xDC,  # JMP/JML and indirect forms
    0x00, 0x02,              # BRK, COP
    0x40,                    # RTI
}
# Stack / frame manipulation. A routine that touches the stack beyond its own
# RTS cannot be tested from synthetic state: 00:98a0, for instance, does
# `PLX ; PLA ; PHA` to pop its OWN return address and use it as a pointer to
# inline arguments the caller emitted after the JSR. Handed a fabricated return
# address it indexes arbitrary memory and writes out of bounds -- a harness
# artefact, not a codegen bug, but indistinguishable from one if allowed
# through. This idiom is common in this ROM (the COP dispatcher does it too).
STACKY = {
    0x68, 0xFA, 0x7A, 0xAB, 0x2B, 0x28,        # PLA PLX PLY PLB PLD PLP
    0x3B, 0xBA, 0x1B, 0x9A,                    # TSC TSX TCS TXS
    0xD4, 0x62, 0xF4,                          # PEI PER PEA
    0x63, 0x83, 0xA3, 0xC3, 0xE3, 0x03, 0x23, 0x43,   # stack-relative
    0x73, 0x93, 0xB3, 0xD3, 0xF3, 0x13, 0x33, 0x53,   # (dp,S),Y
}

# Absolute-addressing opcodes whose operand we can check against the register
# window. Not exhaustive -- indexed/long forms are treated conservatively by
# rejecting any operand in the window regardless of mode.
ABS_OPS = {
    0xAD, 0x8D, 0xAE, 0x8E, 0xAC, 0x8C, 0x2D, 0x0D, 0x4D, 0xCD, 0x6D, 0xED,
    0x2C, 0x9C, 0xEE, 0xCE, 0x0E, 0x4E, 0xBD, 0x9D, 0xBC, 0x1D, 0x3D, 0x5D,
    0x7D, 0xFD, 0xDD, 0xFE, 0xDE, 0x9E, 0xB9, 0x99, 0xBE,
}

# Opcode length table is width-dependent for immediates; rather than track M/X
# we simply reject any body containing an m/x-dependent immediate, which keeps
# the scan exact without needing a full decoder.
IMM_MX = {0xA9, 0x29, 0x09, 0x49, 0xC9, 0x69, 0xE9, 0x89,
          0xA2, 0xA0, 0xE0, 0xC0}

LEN = {}
for op in range(256):
    LEN[op] = 1
for op in (0xA5, 0x85, 0xA6, 0x86, 0xA4, 0x84, 0x25, 0x05, 0x45, 0xC5, 0x65,
           0xE5, 0x24, 0x64, 0x06, 0xE6, 0xC6, 0x46, 0x26, 0x66, 0x95, 0xB5,
           0xB4, 0x94, 0x74, 0x14, 0x04, 0xD6, 0xF6, 0x16, 0x56, 0x36, 0x76,
           0xB2, 0x92, 0xA1, 0x81, 0xB1, 0x91, 0xA3, 0x83, 0xB3, 0x93, 0xE2,
           0xC2, 0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0, 0x80, 0xD4,
           0xA7, 0x87, 0xB7, 0x97, 0x67, 0x47, 0x27, 0x07, 0xC7, 0xE7):
    LEN[op] = 2
for op in ABS_OPS | {0x22, 0x20, 0x4C, 0xFC, 0x6C, 0x7C, 0xDC, 0x1C, 0x0C, 0x9B}:
    LEN[op] = 3
for op in (0xAF, 0xBF, 0x8F, 0x9F, 0xCF, 0xDF, 0xEF, 0xFF, 0x0F, 0x1F, 0x2F,
           0x3F, 0x4F, 0x5F, 0x6F, 0x7F, 0x5C, 0x22):
    LEN[op] = 4
for op in (0x54, 0x44):
    LEN[op] = 3
LEN[0x82] = 3  # BRL


def file_off(pc24):
    return (pc24 >> 16) * 0x8000 + ((pc24 & 0xFFFF) - 0x8000)


def main():
    rom = open(ROM, 'rb').read()
    m = json.load(open(MANIFEST))
    nodes = list(m['nodes'].values()) if isinstance(m['nodes'], dict) else m['nodes']

    kept = []
    for f in nodes:
        if f.get('disposition') != 'aot_eligible':
            continue
        lo, hi = f.get('min_pc24'), f.get('max_pc24')
        if lo is None or hi is None or hi <= lo:
            continue
        if (lo & 0xFFFF) < 0x8000 or (lo >> 16) > 0x0F:
            continue
        if hi - lo > 128:
            continue

        a, b = file_off(lo), file_off(hi)
        if b + 4 > len(rom):
            continue

        ok, p = True, a
        while p <= b:
            op = rom[p]
            if op in TRANSFER or op in IMM_MX or op in STACKY:
                ok = False
                break
            if op in ABS_OPS:
                operand = rom[p + 1] | (rom[p + 2] << 8)
                if 0x2000 <= operand <= 0x5FFF:   # hardware window
                    ok = False
                    break
            if op == 0x60 or op == 0x6B:          # RTS/RTL: end of body
                break
            p += LEN[op]
        if ok:
            kept.append((lo, hi))

    kept = sorted(set(kept))   # M/X variants share one pc24; test each once
    print('/* Auto-generated by tools/select_pure_leaves.py -- do not hand-edit.')
    print(' *')
    print(' * AOT bodies that are pure leaves: no calls or non-local transfers,')
    print(' * no hardware-register access, small extent. These are the routines')
    print(' * where synthetic entry state is a fair test -- see the tool for why')
    print(' * that restriction exists. */')
    print('#define SIMCITY_PURE_LEAF_COUNT %d' % len(kept))
    print('static const struct { unsigned pc24, end; } kSimCityPureLeaves[] = {')
    for lo, hi in kept:
        print('    { 0x%06Xu, 0x%06Xu },' % (lo, hi))
    print('};')
    print('/* selected %d of %d AOT-eligible nodes */'
          % (len(kept), sum(1 for f in nodes if f.get('disposition') == 'aot_eligible')),
          file=sys.stderr)


if __name__ == '__main__':
    main()
