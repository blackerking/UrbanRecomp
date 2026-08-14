#!/usr/bin/env python3
"""Propose `exit_mx_at` directives for the callees the solver cannot prove.

This is the third piece of "measure and check" (docs/OPEN_QUESTIONS.md A1/F).
`mx_exit_report.py` measures, `mx_exit_check.py` verifies the analyzer against
the measurement, and this turns a measurement into a directive -- but only
where the measurement is unambiguous, and it says out loud where it is not.

21 callees block 43 nodes and 2,861 instructions, the whole difference between
94.83% and 99.06% AOT. None of them is undecodable; the solver simply has no
fixed point, because the bank-01 UI handlers dispatch to each other in a cycle
and each is unproven because the other four are. The machine has no such
problem -- it just runs them.

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

Entry variant is read at the **call site**, not the callee's target address: a
JSR changes neither M nor X, so the site's own width is that site's entry
variant, while the target address unions every caller.

    python tools/mx_exit_propose.py <mx_bitmap> [coverage_bitmap] [manifest]
"""
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

    def body_of(target):
        i = entries.index(target)
        end = entries[i + 1] - 1 if i + 1 < len(entries) else target
        if (end >> 16) != (target >> 16):          # next entry is another bank
            end = (target & 0xFF0000) | 0xFFFF
        return target, end

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
        lo, hi = body_of(target)
        seen, observed, total = set(), 0, 0
        for pc in returns:
            if lo <= pc <= hi:
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
