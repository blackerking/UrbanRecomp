#!/usr/bin/env python3
"""Find every call site whose callee skips bytes after the JSR/JSL.

Empirical, not static: it uses a recorded execution bitmap
(`SC_PC_BITMAP_BANK=all`) and looks for the signature of an inline-argument
convention --

    the call executed, the byte immediately after it never executed,
    and a byte a little further on did.

The CPU can only get from the call to that later byte if the callee adjusted
its return address, so the bytes in between are data the decoder must not read
as code. That is exactly the class of bug in docs/UPSTREAM_inline_args.md.

Why empirical: the static form of this question is "does this callee adjust its
return address", which needs a decoder that understands every way of expressing
it -- and getting that wrong in the conservative direction is how the upstream
detector missed four routines and 61 call sites here. A coverage bitmap has no
such blind spot: it records what the CPU actually did.

    python tools/find_inline_args.py [coverage_union.bin]
"""
import collections
import sys

from find_rom import find_rom  # noqa: E402  (tools/ is sys.path[0])
ROM = find_rom('us', required=False) or 'us.sfc'
DEFAULT_BITMAP = 'coverage_union.bin'
CODE_BANKS = range(6)
MAX_SKIP = 16          # beyond this it is not an inline-argument convention


def main():
    bitmap_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BITMAP
    rom = open(ROM, 'rb').read()
    bm = open(bitmap_path, 'rb').read()

    def executed(bank, addr):
        if addr < 0x8000 or bank >= 16:
            return False
        i = addr - 0x8000
        return bool(bm[bank * 4096 + (i >> 3)] & (1 << (i & 7)))

    def file_off(bank, addr):
        return bank * 0x8000 + (addr - 0x8000)

    hits = []
    for bank in CODE_BANKS:
        for addr in range(0x8000, 0x10000):
            if not executed(bank, addr):
                continue
            op = rom[file_off(bank, addr)]
            if op == 0x20:      # JSR abs
                size, tgt = 3, rom[file_off(bank, addr) + 1] | (rom[file_off(bank, addr) + 2] << 8)
                target = (bank << 16) | tgt
            elif op == 0x22:    # JSL long
                o = file_off(bank, addr)
                size = 4
                target = rom[o + 1] | (rom[o + 2] << 8) | (rom[o + 3] << 16)
            else:
                continue

            nxt = addr + size
            if nxt >= 0x10000 or executed(bank, nxt):
                continue        # normal: control resumed right after the call

            # The byte after the call never ran. Find where it did resume.
            for skip in range(1, MAX_SKIP + 1):
                q = nxt + skip
                if q >= 0x10000:
                    break
                if executed(bank, q):
                    hits.append(((bank << 16) | addr, target, skip))
                    break

    by_target = collections.defaultdict(list)
    for site, target, skip in hits:
        by_target[(target, skip)].append(site)

    print('call sites whose callee provably skips bytes after the call')
    print('(call executed, following byte never executed, a later byte executed)\n')
    print('%-12s %-6s %-6s  %s' % ('callee', 'skip', 'sites', 'example call sites'))
    total = 0
    for (target, skip), sites in sorted(by_target.items(), key=lambda kv: -len(kv[1])):
        total += len(sites)
        ex = ' '.join('%02X:%04X' % (s >> 16, s & 0xFFFF) for s in sites[:3])
        print('%02X:%04X     %-6d %-6d  %s%s'
              % (target >> 16, target & 0xFFFF, skip, len(sites), ex,
                 ' ...' if len(sites) > 3 else ''))
    print('\n%d call sites across %d distinct (callee, skip) pairs'
          % (total, len(by_target)))


if __name__ == '__main__':
    main()
