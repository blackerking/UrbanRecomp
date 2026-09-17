"""Disassemble using an SC_MX_BITMAP capture, decoding each address at the
width it ACTUALLY ran at.

dis_mx.py --starts fixes one (m,x) for the whole range, which is fine for a
routine that never changes width. It is useless for real code: 02:909d-9105
alternates m0x0/m0x1/m1x0 within fifty bytes, and a fixed width renders most
of it as garbage -- the same class of misreading that made 02:8b00 look like
data until the bitmap was consulted.

SC_MX_BITMAP writes four planes (mx = m<<1 | x), each 64 banks x 4096 bytes.
An address marked in plane k was executed with those widths, so the plane IS
the alignment. Where an address appears in several planes the first is shown,
with the others noted -- that means the routine is entered at both widths and
is worth a second look.

Usage:  python tools/dis_cov.py <bank> <start> <end> --mx mx_load.bin                                [--rom ROM] [--all-planes]
"""
import argparse
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dis_mx import decode_insn, file_off, fmt

PLANE = 64 * 4096
NAMES = ['m0x0', 'm0x1', 'm1x0', 'm1x1']


def load_planes(path):
    d = open(path, 'rb').read()
    if len(d) != 4 * PLANE:
        raise SystemExit(f'{path}: expected {4 * PLANE} bytes, got {len(d)}')
    return [d[i * PLANE:(i + 1) * PLANE] for i in range(4)]


def marked(plane, bank, addr):
    i = addr - 0x8000
    if i < 0 or i >= 0x8000:
        return False
    return bool(plane[bank * 4096 + (i >> 3)] & (1 << (i & 7)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bank')
    ap.add_argument('start')
    ap.add_argument('end')
    ap.add_argument('--mx', required=True, help='SC_MX_BITMAP output')
    ap.add_argument('--rom', default=None, help='the US ROM (found by contents if omitted)')
    ap.add_argument('--all-planes', action='store_true',
                    help='list every plane an address ran at, not just the first')
    a = ap.parse_args()

    bank = int(a.bank, 16)
    lo = int(a.start, 16)
    hi = int(a.end, 16)
    if a.rom is None:
        from find_rom import find_rom
        a.rom = find_rom('us')
    rom = open(a.rom, 'rb').read()
    planes = load_planes(a.mx)

    shown = 0
    for addr in range(lo, hi):
        hits = [k for k, pl in enumerate(planes) if marked(pl, bank, addr)]
        if not hits:
            continue
        k = hits[0]
        m, x = k >> 1, k & 1
        ins = decode_insn(rom, file_off(bank, addr), addr, bank, m=m, x=x)
        note = ''
        if len(hits) > 1:
            note = '   ; also ' + ','.join(NAMES[h] for h in hits[1:])
        if ins is None:
            print(f'{bank:02x}:{addr:04x}  {NAMES[k]}  .byte '
                  f'${rom[file_off(bank, addr)]:02x}{note}')
        else:
            print(f'{bank:02x}:{addr:04x}  {NAMES[k]}  {fmt(ins, bank)}{note}')
        shown += 1
    print(f'; {shown} executed instructions in {bank:02x}:{lo:04x}-{hi:04x}',
          file=sys.stderr)


if __name__ == '__main__':
    main()
