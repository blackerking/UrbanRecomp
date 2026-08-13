#!/usr/bin/env python3
"""Width-tracking 65816 disassembler.

`tools/dis65816.py` does not track SEP/REP, so every immediate is guessed and
one wrong guess desynchronises the rest of the listing. That has already cost
this project real time -- `00:90dd`'s `CMP #$ff` was read as a 16-bit compare,
and `03:8004`'s `LDA #$03 ; PHA ; PLB` came out as `LDA #$4803`. This tool
carries m/x through SEP/REP so immediates are the width the CPU actually used.

It reuses the recompiler's own opcode table rather than a second copy, so a
listing here decodes exactly as the analyzer does.

Two extra cross-checks, both cheap and both worth having:

  * with a coverage bitmap, every line is marked `*` if that address was
    executed. A line with an operand byte marked executed, or a `*` line the
    linear walk never reaches, means the widths are wrong -- the bitmap
    records real instruction starts, so it is ground truth for alignment.
  * `--starts` ignores the walk entirely and decodes only at addresses the
    bitmap marks, which recovers the true boundaries even where a SEP/REP
    arrives from a caller rather than from the listed code.

    python tools/dis_mx.py 03 8000 815e [--m 1] [--x 1]
                           [--cov coverage_union.bin] [--starts]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'snesrecomp'))
from recompiler.snes65816 import MODE_STR, IMM, REL, REL16, ABS, ABS_X, ABS_Y  # noqa: E402
from recompiler.v2.decoder import decode_insn  # noqa: E402

ROM = 'simcity.sfc'

# Callees that consume bytes embedded after the call and adjust their own
# return address, so the caller resumes past them. Established empirically in
# docs/UPSTREAM_inline_args.md (call executed, following byte never executed,
# a later byte executed) and fixed in the recompiler's decoder. Without this
# the listing decodes the operand bytes as instructions and desynchronises --
# the same failure the width tracking above exists to prevent.
INLINE_ARG_CALLEES = {
    0x0098A0: 2,
    0x03A2F5: 3,
    0x03A421: 3,
    0x03A3CF: 3,
    0x03A350: 3,
}


def file_off(bank, addr):
    return bank * 0x8000 + (addr - 0x8000)


# MODE_STR names the mode ('abs,x', '(dp),y'); it is not a format template,
# so the operand is placed by substituting the leading mode word.
_OPERAND_SLOT = {
    'imp': '', 'acc': ' A',
    'imm': ' #${v}', 'dp': ' ${v}', 'dp,x': ' ${v},X', 'dp,y': ' ${v},Y',
    'abs': ' ${v}', 'abs,x': ' ${v},X', 'abs,y': ' ${v},Y',
    'long': ' ${v}', 'long,x': ' ${v},X',
    'rel': ' ${v}', 'rel16': ' ${v}', 'stk': ' ${v},S',
    '(abs)': ' (${v})', '(abs,x)': ' (${v},X)',
    '(dp),y': ' (${v}),Y', '[dp],y': ' [${v}],Y', '[dp]': ' [${v}]',
    '(dp,x)': ' (${v},X)', '(dp)': ' (${v})', '(stk,S),Y': ' (${v},S),Y',
}


def fmt(insn, bank):
    """Render one instruction with its operand at the CPU's width."""
    name = MODE_STR.get(insn.mode, '')
    slot = _OPERAND_SLOT.get(name)
    if slot is None:
        return f'{insn.mnem} {name}?${insn.operand:x}'
    if not slot:
        return insn.mnem
    digits = max(2, (insn.length - 1) * 2)
    if insn.mode in (REL, REL16):
        digits = 4          # operand is the resolved branch target
    return insn.mnem + slot.replace('{v}', f'{insn.operand:0{digits}x}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bank')
    ap.add_argument('start')
    ap.add_argument('end')
    ap.add_argument('--m', type=int, default=1)
    ap.add_argument('--x', type=int, default=1)
    ap.add_argument('--rom', default=ROM)
    ap.add_argument('--cov', default=None)
    ap.add_argument('--starts', action='store_true',
                    help='decode only at bitmap-marked addresses')
    a = ap.parse_args()
    bank = int(a.bank, 16)
    pc = int(a.start, 16)
    end = int(a.end, 16)
    rom = open(a.rom, 'rb').read()

    cov = None
    if a.cov:
        bm = open(a.cov, 'rb').read()

        def cov(addr):
            i = addr - 0x8000
            return bool(bm[bank * 4096 + (i >> 3)] & (1 << (i & 7)))

    m, x = a.m & 1, a.x & 1
    unreached = []
    while pc < end:
        if a.starts and cov and not cov(pc):
            pc += 1
            continue
        off = file_off(bank, pc)
        insn = decode_insn(rom, off, pc, bank, m=m, x=x)
        if insn is None:
            print(f'{bank:02x}:{pc:04x}  {rom[off]:02x}           .byte ${rom[off]:02x}')
            pc += 1
            continue
        raw = ' '.join(f'{rom[off + i]:02x}' for i in range(insn.length))
        mark = ''
        if cov:
            mark = '*' if cov(pc) else ' '
            for i in range(1, insn.length):
                if cov(pc + i):
                    mark = '!'   # operand byte executed -> misaligned widths
                    unreached.append(pc + i)
        print(f'{bank:02x}:{pc:04x} {mark} {raw:<12} m{m}x{x}  {fmt(insn, bank)}')
        # Inline arguments: the callee skips them, so the caller must too.
        skip = 0
        if insn.mnem == 'JSR' and insn.mode == ABS:
            skip = INLINE_ARG_CALLEES.get((bank << 16) | (insn.operand & 0xFFFF), 0)
        elif insn.mnem == 'JSL':
            skip = INLINE_ARG_CALLEES.get(insn.operand & 0xFFFFFF, 0)
        if skip:
            o2 = file_off(bank, pc + insn.length)
            data = ' '.join(f'{rom[o2 + i]:02x}' for i in range(skip))
            print(f'{bank:02x}:{pc + insn.length:04x}   {data:<12} '
                  f'      .byte {data}   ; inline args')
            pc += insn.length + skip
            if unreached:
                unreached = [u for u in unreached
                             if not (pc - skip <= u < pc)]
            continue
        # Carry the width flags forward.
        if insn.mnem == 'SEP':
            if insn.operand & 0x20:
                m = 1
            if insn.operand & 0x10:
                x = 1
        elif insn.mnem == 'REP':
            if insn.operand & 0x20:
                m = 0
            if insn.operand & 0x10:
                x = 0
        pc += insn.length
    if unreached:
        print(f'\n!! {len(unreached)} operand byte(s) marked executed: '
              f'{", ".join(f"{u:04x}" for u in unreached[:12])}')
        print('   the widths used here disagree with the recorded run.')


if __name__ == '__main__':
    main()
