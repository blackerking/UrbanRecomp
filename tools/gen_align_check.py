#!/usr/bin/env python3
"""Check that emitted code decodes on the boundaries the CPU actually used.

An `exit_mx_at` directive is an assertion about a width, and the failure mode
of a wrong one is silent: the caller resumes decoding after the call at the
wrong width, every following immediate is sized wrong, and the instruction
stream desynchronises from there. That is precisely the inline-argument bug
(docs/UPSTREAM_inline_args.md) -- clean-looking `aot_eligible` bodies full of
instructions the ROM never executes.

The AOT differential cannot catch it here, because this host still runs
everything on the interpreter (`src/main.c`: "this host bypasses common_rtl.c
entirely"). The `SimCitySNESRecompAOT` binary links the generated banks but
does not execute them, so byte-identical WRAM between the two builds says
nothing about whether the emitted C is right.

What does catch it is the execution bitmap, used the same way the inline-arg
investigation used it. The bitmap records instruction *fetches*, so decoding
each executed address at the width it was executed in yields, for free, the
set of bytes that are definitely operands. An emitted label sitting on one of
those bytes is a decode desynchronisation, and no interpretation of the
listing can make it benign.

The check is one-sided on purpose: a label at an address the bitmap never
recorded proves nothing (the path may simply never have run), so only
positively-known operand bytes are treated as evidence.

    python tools/gen_align_check.py <mx_bitmap> [gen_dir]
"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'snesrecomp'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from recompiler.v2.decoder import decode_insn          # noqa: E402
from mx_exit_check import Bitmap, PLANE                # noqa: E402

ROM = 'simcity.sfc'
LABEL = re.compile(r'^\s*L_([0-9A-Fa-f]{4})_M([01])X([01]):', re.M)


def main():
    mx = Bitmap(open(sys.argv[1], 'rb').read())
    gen = sys.argv[2] if len(sys.argv) > 2 else 'src/gen'
    rom = open(ROM, 'rb').read()

    # Bytes that are definitely operands: interior bytes of an executed
    # instruction. Where an address ran at several widths the immediate can
    # differ in size, so only the bytes interior under *every* observed width
    # are claimed -- the shortest decode wins, which keeps the check sound.
    operand = set()
    starts = 0
    for bank in range(64):
        for byte_i in range(4096):
            acc = 0
            for p in range(4):
                acc |= mx.b[p * PLANE + bank * 4096 + byte_i]
            if not acc:
                continue
            for bit in range(8):
                if not (acc & (1 << bit)):
                    continue
                addr = 0x8000 + byte_i * 8 + bit
                pc24 = (bank << 16) | addr
                starts += 1
                lengths = []
                off = bank * 0x8000 + (addr - 0x8000)
                for p in mx.widths(pc24):
                    ins = decode_insn(rom, off, addr, bank, m=(p >> 1), x=(p & 1))
                    if ins is not None:
                        lengths.append(ins.length)
                if lengths:
                    for k in range(1, min(lengths)):
                        operand.add(pc24 + k)

    print(f'executed instruction starts : {starts}')
    print(f'bytes proven to be operands : {len(operand)}')

    bad, labels = [], 0
    for name in sorted(os.listdir(gen)):
        if not name.endswith('.c'):
            continue
        bank = re.match(r'bank([0-9a-f]{2})', name)
        if not bank:
            continue
        bank = int(bank.group(1), 16)
        text = open(os.path.join(gen, name), encoding='utf-8', errors='replace').read()
        for m in LABEL.finditer(text):
            labels += 1
            pc24 = (bank << 16) | int(m.group(1), 16)
            if pc24 in operand:
                line = text.count('\n', 0, m.start()) + 1
                bad.append((name, line, pc24, m.group(2), m.group(3)))

    print(f'emitted block labels        : {labels}')
    print()
    if not bad:
        print('OK -- no emitted label lands on a byte the CPU used as an operand.')
        return 0
    print(f'DESYNCHRONISED -- {len(bad)} label(s) on known operand bytes:')
    for name, line, pc24, m_, x_ in bad:
        print(f'  {name}:{line}  L_{pc24 & 0xFFFF:04X}_M{m_}X{x_} '
              f'= {pc24 >> 16:02X}:{pc24 & 0xFFFF:04X}')
    return 1


if __name__ == '__main__':
    sys.exit(main())
