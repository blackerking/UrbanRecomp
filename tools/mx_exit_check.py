#!/usr/bin/env python3
"""Check the analyzer's *published* exit M/X against what the machine does.

`mx_exit_report.py` is the measurement half of "measure and check"
(docs/OPEN_QUESTIONS.md F): it reports the widths observed at the return
addresses of callees the solver could *not* prove. This is the other half --
it takes the callees the solver *did* publish an exit mode for and asks
whether the machine agrees. Where it does, the solver is sound at that site;
where it does not, either the published exit or the demand's variant key is
wrong, and the entry-width pass below says which.

Four confounds make the naive comparison meaningless, and all four are
corrected here rather than left as caveats. The uncorrected run reports 40
"mismatches" that are nothing of the sort (F1), and correcting only the first
two still leaves 26 that are also nothing of the sort:

  1. **Inline arguments.** Five routines consume bytes after their JSR and
     resume past them, across 125 call sites. The return address is
     `site + len + skip`, not `site + len`; reading the naive address samples
     an operand byte. Table below, from docs/UPSTREAM_inline_args.md.

  2. **Reachability.** A return address is not reached only by returning. If
     an executed branch, jump or a *different* call can also land on it, the
     observed width set is a union over every way in, and says nothing about
     this callee's exit. Those sites are excluded, not explained away.

  3. **The call site must itself have executed.** Otherwise nothing ever
     returned to that address and whatever width was recorded there arrived
     by some other path -- the same confound as (2), by a route no static
     branch target reveals, such as a fall-through.

  4. **Which callee variant the machine actually entered.** This is the one
     that produced the 26. A site carries a *demand per variant*: the
     analyzer cannot prove the entry width statically, so it emits both, and
     for an X-transparent callee it correctly publishes exit x=1 for the
     x=1 variant and x=0 for the x=0 one. Comparing the machine against
     whichever demand row was enumerated first therefore accuses the solver
     of a disagreement with a variant that never ran.

     The fix needs no new instrumentation. **A JSR does not change M or X**,
     so the width recorded at the call site's own address *is* the entry
     variant for that site. Looking the published exit up under that key
     compares like with like.

The entry width at the callee's target address is reported alongside as a
cross-check, but it is deliberately not what the comparison keys on: the
target is a union over every caller, while the call site is specific to this
one.

    python tools/mx_exit_check.py <mx_bitmap> [coverage_bitmap] [manifest]

`coverage_bitmap` is a plain SC_PC_BITMAP_BANK=all dump used only to decide
which branch/jump sites executed; it defaults to the M/X bitmap's own union,
which is correct but excludes fewer sites.
"""
import collections
import json
import re
import sys

MANIFEST = 'src/gen/program_manifest.json'
ROM = 'simcity.sfc'
PLANE = 64 * 4096
NAMES = {0: 'm0x0', 1: 'm0x1', 2: 'm1x0', 3: 'm1x1'}

# Routines that consume inline bytes after the call and resume past them.
# docs/UPSTREAM_inline_args.md -- found empirically from a coverage bitmap
# (call executes, next byte never does, a byte further on does), not by
# pattern-matching the ROM, which is how they were missed upstream.
INLINE_SKIP = {0x0098A0: 2, 0x03A2F5: 3, 0x03A421: 3, 0x03A3CF: 3, 0x03A350: 3}

BRANCH8 = {0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0, 0x80}
CALL_LEN = {0x20: 3, 0x22: 4}


def key_of(pc24, m, x):
    return f'{pc24:06X}:M{m}X{x}'


class Bitmap:
    def __init__(self, blob):
        if len(blob) != 4 * PLANE:
            sys.exit(f'M/X bitmap must be {4 * PLANE} bytes, got {len(blob)}')
        self.b = blob

    def widths(self, pc24):
        """The (m,x) planes in which pc24 was ever executed."""
        bank, addr = (pc24 >> 16) & 0xFF, pc24 & 0xFFFF
        if addr < 0x8000 or bank >= 64:
            return ()
        i = addr - 0x8000
        return tuple(mx for mx in range(4)
                     if self.b[mx * PLANE + bank * 4096 + (i >> 3)] & (1 << (i & 7)))


def load_executed(path, mx):
    """Set of executed PCs, from a plain coverage bitmap or the M/X union."""
    if path:
        blob = open(path, 'rb').read()
        if len(blob) != PLANE:
            sys.exit(f'coverage bitmap must be {PLANE} bytes, got {len(blob)}')
    else:
        blob = bytes(mx.b[i] | mx.b[i + PLANE] | mx.b[i + 2 * PLANE] | mx.b[i + 3 * PLANE]
                     for i in range(PLANE))
    out = set()
    for bank in range(64):
        for byte_i in range(4096):
            v = blob[bank * 4096 + byte_i]
            if not v:
                continue
            for bit in range(8):
                if v & (1 << bit):
                    out.add((bank << 16) | (0x8000 + byte_i * 8 + bit))
    return out


def landing_sites(rom, executed):
    """Addresses an executed branch, jump or call can transfer control to.

    A return address that is also one of these is not measuring a return.
    Indirect dispatch (JMP/JSR (abs,X) and friends) has no static target, so
    those sites are counted but cannot be excluded -- reported, not hidden.
    """
    targets = {'branch': set(), 'jump': set(), 'call': set()}
    indirect = 0

    def rd(pc24, n):
        bank, addr = pc24 >> 16, pc24 & 0xFFFF
        off = bank * 0x8000 + (addr - 0x8000)
        return rom[off:off + n]

    for pc in executed:
        bank = pc >> 16
        op = rd(pc, 1)
        if not op:
            continue
        op = op[0]
        if op in BRANCH8:
            d = rd(pc + 1, 1)[0]
            d = d - 256 if d >= 128 else d
            targets['branch'].add((bank << 16) | ((pc + 2 + d) & 0xFFFF))
        elif op == 0x82:                                    # BRL
            lo, hi = rd(pc + 1, 2)
            d = lo | (hi << 8)
            d = d - 65536 if d >= 32768 else d
            targets['branch'].add((bank << 16) | ((pc + 3 + d) & 0xFFFF))
        elif op == 0x4C:                                    # JMP abs
            lo, hi = rd(pc + 1, 2)
            targets['jump'].add((bank << 16) | lo | (hi << 8))
        elif op == 0x5C:                                    # JMP long
            lo, hi, bk = rd(pc + 1, 3)
            targets['jump'].add((bk << 16) | lo | (hi << 8))
        elif op == 0x20:                                    # JSR abs
            lo, hi = rd(pc + 1, 2)
            targets['call'].add((bank << 16) | lo | (hi << 8))
        elif op == 0x22:                                    # JSL long
            lo, hi, bk = rd(pc + 1, 3)
            targets['call'].add((bk << 16) | lo | (hi << 8))
        elif op in (0x6C, 0x7C, 0xDC, 0xFC):
            indirect += 1
    return targets, indirect


def main():
    mx = Bitmap(open(sys.argv[1], 'rb').read())
    cov_path = sys.argv[2] if len(sys.argv) > 2 else None
    manifest = json.load(open(sys.argv[3] if len(sys.argv) > 3 else MANIFEST))
    rom = open(ROM, 'rb').read()

    exit_modes = manifest['exit_modes']
    exit_sets = manifest['exit_mode_sets']

    executed = load_executed(cov_path, mx)
    targets, indirect = landing_sites(rom, executed)
    landable = targets['branch'] | targets['jump'] | targets['call']

    def published(pc24, m, x):
        """The set of exit widths the analyzer publishes for a callee variant."""
        k = key_of(pc24, m, x)
        if k in exit_modes:
            e = exit_modes[k]
            return {(e['m'] << 1) | e['x']}
        if k in exit_sets:
            return {(e['m'] << 1) | e['x'] for e in exit_sets[k]}
        return None

    stats = collections.Counter()
    agree, mismatch = [], []

    # One row per (site, callee), not per demand: the demands at a site differ
    # only by the variant they assume, and the machine picks one of them.
    sites = {}
    for node in manifest['nodes'].values():
        for d in node.get('demands') or []:
            if d.get('kind') == 'direct_call':
                sites.setdefault((d['site_pc24'], d['target']['pc24']), None)

    for site, pc24 in sorted(sites):
        off = (site >> 16) * 0x8000 + ((site & 0xFFFF) - 0x8000)
        n = CALL_LEN.get(rom[off]) if 0 <= off < len(rom) else None
        if n is None:
            stats['skip: site is not a direct JSR/JSL'] += 1
            continue

        # Confound 4: the width at the call site names the entry variant,
        # because JSR/JSL leave M and X alone.
        entered = mx.widths(site)
        if not entered:
            stats['skip: call site never executed'] += 1
            continue

        pub = set()
        unpublished = []
        for e in entered:
            p = published(pc24, e >> 1, e & 1)
            if p is None:
                unpublished.append(e)
            else:
                pub |= p
        if unpublished:
            stats['skip: an entered callee variant has no published exit'] += 1
            continue

        ret = site + n + INLINE_SKIP.get(pc24, 0)
        if pc24 in INLINE_SKIP:
            stats['corrected: inline-argument return address'] += 1

        obs = mx.widths(ret)
        if not obs:
            stats['skip: return address never executed'] += 1
            continue
        if ret in landable:
            stats['excluded: return address is also a branch/jump/call target'] += 1
            continue

        row = (site, pc24, entered, set(obs), pub, mx.widths(pc24))
        if set(obs) <= pub:
            agree.append(row)
        else:
            mismatch.append(row)

    print(f'executed instruction addresses     : {len(executed)}')
    print(f'  branch targets among them        : {len(targets["branch"])}')
    print(f'  jump targets                     : {len(targets["jump"])}')
    print(f'  call targets                     : {len(targets["call"])}')
    print(f'  indirect dispatch sites (no static target, cannot exclude): {indirect}')
    print()
    for k in sorted(stats):
        print(f'  {k}: {stats[k]}')
    print()
    print(f'checked {len(agree) + len(mismatch)}   '
          f'agree {len(agree)}   mismatch {len(mismatch)}')

    # Keying the lookup on the entered width would make the check vacuous for
    # any callee that simply preserves M and X: published exit == entry width
    # == observed exit, and agreement is guaranteed by construction. Count the
    # rows where the analyzer publishes an exit that *differs* from the width
    # the call was made at -- those are the ones with something to be wrong
    # about, and the total is only meaningful next to them.
    nontrivial = [r for r in agree + mismatch if r[4] != set(r[2])]
    changed = [r for r in nontrivial if len(r[4]) == 1 and len(r[2]) == 1]
    print(f'  of those, {len(nontrivial)} publish an exit width different from '
          f'the entry width ({len(changed)} a single definite change)')
    if nontrivial:
        moves = collections.Counter()
        for _, _, entered, _, pub, _ in nontrivial:
            for e in entered:
                for p in pub:
                    if e != p:
                        moves[(NAMES[e], NAMES[p])] += 1
        print('  entry -> published exit transitions actually exercised:')
        for (a, b), v in moves.most_common(8):
            print(f'    {a} -> {b}  x{v}')

    if not mismatch:
        return

    def fmt(s):
        return ' '.join(NAMES[v] for v in sorted(s)) or '-'

    print('\nmismatches -- the machine exits in a width the analyzer does not '
          'publish for the variant it entered:')
    print(f'{"site":>9} {"callee":>9}  {"entered as":<14} {"observed exit":<14} '
          f'{"published exit":<14} {"entry @target":<14}')
    for site, pc24, entered, obs, pub, entry in sorted(mismatch):
        print(f'{site >> 16:02X}:{site & 0xFFFF:04X} {pc24 >> 16:02X}:{pc24 & 0xFFFF:04X}  '
              f'{fmt(entered):<14} {fmt(obs):<14} {fmt(pub):<14} {fmt(entry):<14}')

    # If every mismatch moves the same flag the same way it is one systematic
    # error, not N independent solver bugs. Worth stating either way, because
    # the raw count reads like a finding on its own.
    diffs = collections.Counter()
    for _, _, _, obs, pub, _ in mismatch:
        for o in obs - pub:
            for p in pub:
                if (o ^ p) == 1:
                    diffs[('x', p & 1, o & 1)] += 1
                elif (o ^ p) == 2:
                    diffs[('m', p >> 1, o >> 1)] += 1
                else:
                    diffs[('both', p, o)] += 1
    print('\n(flag, published, observed) differences:')
    for k, v in diffs.most_common():
        print(f'  {v:4d}  {k}')


if __name__ == '__main__':
    main()
