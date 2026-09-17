#!/usr/bin/env python3
"""Report the exit M/X widths the machine actually returns in, per callee.

The analyzer leaves ~300 of 1584 nodes without published exit M/X, and an
unpublished exit truncates every caller of that node, which is essentially the
whole remaining LLE-only set (docs/OPEN_QUESTIONS.md A1). The blocker is a
fixpoint over mutually recursive dispatch cycles: nothing is missing
statically, the solver just has no fixed point.

The machine does not have that problem. A call's *return address* executes in
exactly the widths the callee exited with, so recording (pc, m, x) turns the
proof obligation into a measurement -- the same move the execution bitmap
already made for "is this address code".

This reads:
  * `src/gen/program_manifest.json` for `unproven_call_at_SITE_to_TARGET_mMxX`
  * an M/X bitmap from SC_MX_BITMAP (4 x 64 x 4096 bytes, index m<<1|x)

and reports, per unproven callee, the widths observed at its callers' return
addresses.

**This output is evidence, not a patch.** `exit_mx_at` is an unchecked
assertion: declaring a width the ROM does not actually exit in miscompiles
silently, which is the same failure class as the inline-argument bug. A callee
observed exiting in one width across every recording is *consistent with*
having a single exit width, not proof of it -- an unexercised path may exit
differently. Treat a unanimous reading as a candidate to be checked, and treat
a split reading as proof that a single `exit_mx_at` would be wrong.

    python tools/mx_exit_report.py <mx_bitmap> [manifest]
"""
import collections
import json
import re
import sys

MANIFEST = 'src/gen/program_manifest.json'
from find_rom import find_rom  # noqa: E402  (tools/ is sys.path[0])
ROM = find_rom('us', required=False) or 'us.sfc'
SZ = 64 * 4096
NAMES = {0: 'm0x0', 1: 'm0x1', 2: 'm1x0', 3: 'm1x1'}


def main():
    bm = open(sys.argv[1], 'rb').read()
    manifest = sys.argv[2] if len(sys.argv) > 2 else MANIFEST
    rom = open(ROM, 'rb').read()
    nodes = json.load(open(manifest))['nodes']

    def observed(pc24):
        bank, addr = (pc24 >> 16) & 0xFF, pc24 & 0xFFFF
        if addr < 0x8000 or bank >= 64:
            return []
        i = addr - 0x8000
        out = []
        for mx in range(4):
            byte = bm[mx * SZ + bank * 4096 + (i >> 3)]
            if byte & (1 << (i & 7)):
                out.append(mx)
        return out

    def call_length(pc24):
        """JSR abs = 3, JSL long = 4; anything else is not a direct call."""
        bank, addr = (pc24 >> 16) & 0xFF, pc24 & 0xFFFF
        off = bank * 0x8000 + (addr - 0x8000)
        if off >= len(rom):
            return None
        op = rom[off]
        return 3 if op == 0x20 else 4 if op == 0x22 else None

    # target -> set of (site, observed-at-return)
    per_target = collections.defaultdict(list)
    for node in nodes.values():
        if node['disposition'] == 'aot_eligible':
            continue
        for reason in node.get('reasons') or []:
            m = re.match(r'unproven_call_at_([0-9A-F]{6})_to_([0-9A-F]{6})_m([01])x([01])',
                         reason)
            if not m:
                continue
            site = int(m.group(1), 16)
            target = int(m.group(2), 16)
            n = call_length(site)
            if n is None:
                continue                     # indirect dispatch site, no fixed return
            per_target[target].append((site, tuple(observed(site + n))))

    print('callee      sites  observed exit widths at return addresses')
    unanimous = split = unseen = 0
    for target in sorted(per_target):
        seen = [o for _, o in per_target[target] if o]
        sites = len(per_target[target])
        if not seen:
            unseen += 1
            continue
        modes = set()
        for o in seen:
            modes.update(o)
        label = ' '.join(NAMES[m] for m in sorted(modes))
        if len(modes) == 1:
            unanimous += 1
            mark = 'single'
        else:
            split += 1
            mark = 'SPLIT - exit_mx_at cannot express this'
        print(f'  {target >> 16:02X}:{target & 0xFFFF:04X}  {sites:5d}  {label:<20s} {mark}')
    print(f'\n{unanimous} callees observed with a single exit width, '
          f'{split} with more than one, {unseen} never observed returning.')
    print('A single observed width is a candidate to verify, not a proof: an '
          'unexercised path may exit differently.')


if __name__ == '__main__':
    main()
