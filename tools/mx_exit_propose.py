#!/usr/bin/env python3
"""Propose `exit_mx_at` directives for the callees the solver cannot prove.

This is the third piece of "measure and check" (docs/OPEN_QUESTIONS.md A1/F).
`mx_exit_report.py` measures, `mx_exit_check.py` verifies the analyzer against
the measurement, and this turns a measurement into a directive -- but only
where the measurement is unambiguous, and it says out loud where it is not.

None of the blocked callees is undecodable; the solver simply has no fixed
point, because the bank-01 UI handlers dispatch to each other in a cycle and
each is unproven because the other four are. The machine has no such problem
-- it just runs them. That cycle is now closed: all five handlers publish a
measured exit, taking AOT-eligible variants from 1475 to 1531 and executed-code
coverage from 96.5% to 97.6%. Five callees remain, of which 02:8000 is worth
6 nodes and 5,388 instructions on its own.

**The evidence is read at the callee's own RTS/RTL, not at its callers'
return addresses.** The return-address reading, which is what
`mx_exit_report.py` does, cannot attribute anything for these callees: 44 of
the 60 call sites are `JSR (abs,X)`, whose return address is fixed but whose
*target* is chosen at runtime, so the widths seen there are a union over
whichever handlers the dispatch happened to pick. The width recorded at an
`RTS`/`RTL` has no such ambiguity -- it is the exit width of the routine that
instruction belongs to, by definition. The return-address reading is kept
alongside as corroboration where a direct call does exist.

Two properties of the cfg directive constrain what can honestly be emitted:

  * `exit_mx_at <addr> <m> <x>` **broadcasts** one exit to every entry variant
    of the callee (`_rebuild_callee_exit_mx` in v2_regen.py). The autoroute
    docstring is explicit that broadcasting a variant-dependent exit "poisons
    non-default callers". So a callee is only proposable here if every entry
    variant observed exits the same way.

  * A single exit width must actually be single. A callee seen exiting two
    ways cannot be expressed at all, and saying so is the point -- `00:C3F9`
    is the case that would have miscompiled silently had the widths been
    guessed from "it looks like it always returns m0x0".

**Never feed an `SC_FREEZE` run into this.** Holding a WRAM byte at a value
the ROM never holds there drives execution into states it never reaches, and
the widths recorded in them are not evidence about anything. Measured: adding
20 frozen runs to a 99-run union turned a clean `mx_exit_check` (1205/1205)
into 2 mismatches, made `gen_align_check` report a desynchronised label that
is fine in every natural run, and recorded executed PCs in bank $18 -- outside
the 512KB ROM image altogether, i.e. the CPU running off into open bus. Freeze
is a fine instrument for answering "is byte X the thing gating behaviour Y";
it is not a way to manufacture coverage.

Entry variant is read at the **call site**, not the callee's target address: a
JSR changes neither M nor X, so the site's own width is that site's entry
variant, while the target address unions every caller.

    python tools/mx_exit_propose.py <mx_bitmap> [coverage_bitmap] [manifest]
"""
import bisect
import collections
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mx_exit_check import (Bitmap, CALL_LEN, INLINE_SKIP, NAMES, MANIFEST, ROM,
                           landing_sites, load_executed)


def main():
    mx = Bitmap(open(sys.argv[1], 'rb').read())
    cov_path = sys.argv[2] if len(sys.argv) > 2 else None
    manifest = json.load(open(sys.argv[3] if len(sys.argv) > 3 else MANIFEST))
    rom = open(ROM, 'rb').read()
    nodes = manifest['nodes']

    executed = load_executed(cov_path, mx)
    targets, _ = landing_sites(rom, executed)
    landable = targets['branch'] | targets['jump'] | targets['call']

    # The callees named by every unproven_call_at_SITE_to_TARGET reason, and
    # the nodes each one truncates -- so the report can say what unblocking it
    # would actually buy.
    sites_of = collections.defaultdict(set)
    blocks = collections.defaultdict(set)
    for k, node in nodes.items():
        if node['disposition'] != 'lle_only':
            continue
        for reason in node.get('reasons') or []:
            m = re.match(r'unproven_call_at_([0-9A-F]{6})_to_([0-9A-F]{6})_m([01])x([01])',
                         reason)
            if m:
                sites_of[int(m.group(2), 16)].add(int(m.group(1), 16))
                blocks[int(m.group(2), 16)].add(k)

    print(f'{len(sites_of)} unproven callees, blocking '
          f'{len(set().union(*blocks.values())) if blocks else 0} nodes '
          f'({sum(nodes[k]["instruction_count"] for k in set().union(*blocks.values())) if blocks else 0} instructions)\n')

    # Every executed RTS/RTL, by address. Restricted to addresses the coverage
    # bitmap marks as executed, which records instruction *fetches* -- so a
    # $60 that is really an operand byte or data cannot be mistaken for a
    # return.
    returns = {pc for pc in executed
               if rom[(pc >> 16) * 0x8000 + ((pc & 0xFFFF) - 0x8000)] in (0x60, 0x6B)}

    # A routine's body runs from its entry to just before the next entry, NOT
    # to its node's max_pc24. Two things break the max_pc24 reading, in
    # opposite directions:
    #
    #   * **Nesting.** 01:AAD5's node spans 541 bytes and physically contains
    #     the whole of 01:AC0E and 01:AC23. Their returns are not AAD5's, and
    #     counting them made AAD5 look like a 3-return routine with 1 measured
    #     -- rejected as PARTIAL when it actually has exactly one return.
    #   * **Truncation.** 01:AC0E's node stops at AC19 because the JSR there
    #     has an unproven exit, so its real return at AC22 lies outside every
    #     node's range and is attributed to whatever encloses it.
    #
    # Bounding by the next entry address handles both, and assumes only that
    # routines are laid out contiguously -- which, if violated, shows up as a
    # contradictory split rather than a wrong directive.
    entries = sorted({int(k.split(':')[0], 16) for k in nodes})

    def extent_of(addr):
        """[addr, next entry above addr). Works for any address, not just an
        entry: a routine can jump into the middle of a region that has no node
        of its own, and that stretch still runs until the next known entry."""
        i = bisect.bisect_right(entries, addr)
        end = entries[i] - 1 if i < len(entries) else addr
        if (end >> 16) != (addr >> 16):            # next entry is another bank
            end = (addr & 0xFF0000) | 0xFFFF
        return addr, end

    def jump_targets(lo, hi):
        """Executed unconditional direct jumps leaving [lo, hi]."""
        out = set()
        for pc in range(lo, hi + 1):
            if pc not in executed:
                continue
            off = (pc >> 16) * 0x8000 + ((pc & 0xFFFF) - 0x8000)
            if off + 4 > len(rom):
                continue
            op = rom[off]
            if op == 0x4C:                                    # JMP abs
                t = (pc & 0xFF0000) | rom[off + 1] | (rom[off + 2] << 8)
            elif op == 0x5C:                                  # JMP long
                t = (rom[off + 3] << 16) | rom[off + 1] | (rom[off + 2] << 8)
            elif op == 0x80:                                  # BRA
                d = rom[off + 1]
                t = (pc & 0xFF0000) | ((pc + 2 + (d - 256 if d >= 128 else d)) & 0xFFFF)
            else:
                continue
            if not (lo <= t <= hi):
                out.add(t)
        return out

    def body_of(target):
        """Address ranges a routine can return from.

        The contiguous [entry, next entry) extent is the starting point, but a
        routine can `JMP` into a shared tail and return from there -- 02:8000
        does exactly that at 02:803C (`JMP $824B`), exiting via 02:8374 in
        m0x1 as well as via its own 02:8195 in m1x1. Missing that made the
        routine look single-exit while its callers plainly returned in two
        widths, which the conflict check caught. Following executed
        unconditional direct jumps recovers the real exit set.

        Conditional branches are deliberately not followed: they stay within
        a routine far more often than not, and chasing them would merge
        unrelated code. An unfollowed edge costs a rejection, never a wrong
        directive.
        """
        ranges, seen, queue = [], set(), [target]
        while queue:
            e = queue.pop()
            if e in seen:
                continue
            seen.add(e)
            lo, hi = extent_of(e)
            ranges.append((lo, hi))
            for t in jump_targets(lo, hi):
                if (t >> 16) == (target >> 16) and t not in seen:
                    queue.append(t)
        return ranges

    def exit_at_returns(target):
        """Widths recorded at the RTS/RTL instructions of the callee's body.

        Returns (widths, observed, total). `total` counts every return the
        *coverage* bitmap says executed, `observed` only those this M/X
        recording actually caught. When they differ the evidence is partial:
        a routine with four return points of which one was measured says
        nothing about the other three, and a single width read off it would
        be a guess wearing a measurement's clothes.
        """
        if target not in entries:
            return set(), 0, 0
        ranges = body_of(target)
        seen, observed, total = set(), 0, 0
        for pc in returns:
            if not any(lo <= pc <= hi for lo, hi in ranges):
                continue
            total += 1
            w = mx.widths(pc)
            if w:
                seen.update(w)
                observed += 1
        return seen, observed, total

    proposals, rejected = [], []
    for target in sorted(sites_of):
        per_entry = collections.defaultdict(set)   # entry mx -> exit mx set
        measured = excluded = dead = 0
        for site in sorted(sites_of[target]):
            off = (site >> 16) * 0x8000 + ((site & 0xFFFF) - 0x8000)
            n = CALL_LEN.get(rom[off]) if 0 <= off < len(rom) else None
            if n is None:
                continue                       # indirect dispatch, no fixed return
            entered = mx.widths(site)
            if not entered:
                dead += 1
                continue
            ret = site + n + INLINE_SKIP.get(target, 0)
            if ret in landable:
                excluded += 1
                continue
            obs = mx.widths(ret)
            if not obs:
                dead += 1
                continue
            measured += 1
            for e in entered:
                per_entry[e].update(obs)

        n_nodes = len(blocks[target])
        n_instr = sum(nodes[k]['instruction_count'] for k in blocks[target])
        rts_exits, n_obs, n_rts = exit_at_returns(target)
        head = (f'{target >> 16:02X}:{target & 0xFFFF:04X}  '
                f'{len(sites_of[target])} sites ({measured} via a direct call, '
                f'{excluded} excluded, {dead} unobserved), '
                f'{n_obs}/{n_rts} executed RTS/RTL measured  '
                f'blocks {n_nodes} nodes / {n_instr} instr')

        corrob = ('  '.join(f'{NAMES[e]}->{"/".join(NAMES[v] for v in sorted(s))}'
                            for e, s in sorted(per_entry.items()))
                  or 'no direct call site observed')

        if not rts_exits:
            rejected.append((head, f'no executed RTS/RTL in the body -- the routine '
                                   f'was never seen returning, so there is no evidence '
                                   f'either way  [callers: {corrob}]'))
            continue

        detail = f'RTS/RTL widths {"/".join(NAMES[v] for v in sorted(rts_exits))}' \
                 f'  [callers: {corrob}]'

        # The return-address reading is independent evidence where it exists.
        # If the two disagree, something in the model is wrong and the callee
        # must not be proposed on either.
        ret_exits = set().union(*per_entry.values()) if per_entry else set()
        if ret_exits and not ret_exits <= rts_exits:
            rejected.append((head, f'CONFLICT: RTS/RTL says '
                                   f'{"/".join(NAMES[v] for v in sorted(rts_exits))} but '
                                   f'callers return in '
                                   f'{"/".join(NAMES[v] for v in sorted(ret_exits))} -- '
                                   f'not proposing on contradictory evidence'))
            continue

        if len(rts_exits) > 1:
            rejected.append((head, f'SPLIT exit: {detail} -- no single '
                                   f'exit_mx_at can be correct for this routine'))
            continue

        # Partial evidence is the failure mode this whole exercise exists to
        # avoid. 02:8000 returns from four places; measuring one of them and
        # broadcasting its width to the routine is exactly the unchecked
        # assertion that hand-written exit_mx_at was rejected for.
        if n_obs < n_rts:
            rejected.append((head, f'PARTIAL: only {n_obs} of {n_rts} executed '
                                   f'return points were measured ({detail}) -- the '
                                   f'unmeasured ones may exit differently'))
            continue
        e = next(iter(rts_exits))
        proposals.append((target, e >> 1, e & 1, head, detail, n_nodes, n_instr))

    print('=== proposable: one exit width, identical across every observed entry variant ===')
    for target, em, ex, head, detail, _, _ in proposals:
        print(f'{head}\n    {detail}\n    exit_mx_at {target:06x} {em} {ex}')
    print(f'\n=== not proposable ({len(rejected)}) ===')
    for head, why in rejected:
        print(f'{head}\n    {why}')

    gained = set().union(*[blocks[t] for t, *_ in proposals]) if proposals else set()
    print(f'\n{len(proposals)} proposable, {len(rejected)} not.')
    print(f'directives would address {len(gained)} of the blocked nodes '
          f'({sum(nodes[k]["instruction_count"] for k in gained)} instructions).')
    print('\nA directive here is a measurement, not a proof: these callees are '
          'observed, not shown, to exit this way. Emit them, regenerate, and '
          'verify with mx_exit_check.py against runs the measurement did NOT '
          'come from -- checking them against their own source data proves '
          'nothing.')

    if len(sys.argv) > 4:
        with open(sys.argv[4], 'w') as f:
            for target, em, ex, _, detail, _, _ in proposals:
                f.write(f'exit_mx_at {target:06x} {em} {ex}   # measured: {detail}\n')
        print(f'\nwrote {len(proposals)} directives to {sys.argv[4]}')


if __name__ == '__main__':
    main()
