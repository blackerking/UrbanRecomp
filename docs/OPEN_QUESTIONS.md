# What is left for investigation

State as of the COP work (AOT share 94.8% analyzed, 96.1% of executed code).
Ordered by value, not by area. Each item says what is actually known, so the
next session does not re-derive it.

---

## A. Recompiler coverage

### A1. The exit-M/X fixpoint — the whole remaining 5.2%

**300 of 1,584 nodes never get exit M/X published** (1,284 do), and an
unpublished exit truncates *every* caller, transitively. This is the single
cause of essentially all remaining LLE-only code.

It is not a decode or table-recovery problem. `038DF1:M0X0` is `aot_eligible`
with an **empty `reasons` list** — clean, compiled — and its caller `03:8000`
is still `lle_only` with `truncated_call_continuation`, purely because
`038DF1` has no entry in the manifest's `exit_modes`. Compilability and
exit-width publication are separate fixpoints.

The shape that defeats the solver is a mutually recursive dispatch cycle.
Bank 01's UI state machine is the clean example: `$01df` selects through two
parallel tables at `01:9d1a` (call) and `01:9d3a` (jump), and the five
handlers `01:A886 / A97C / AA39 / AAD5 / AD54` each dispatch through them to
all five. Each is unproven because the other four are.

Options, best first:

1. An SCC fixed-point solver upstream. `v2_analyze.py` already has an
   `assumptions` set and a comment about a "closed SCC solver", so this was
   anticipated and not finished.
2. **Measure and check.** Record exit M/X per callee from a real run, then
   emit `exit_mx_at` lines *and have the analyzer verify them* against the
   recording. This is the pragmatic path and fits this project's existing
   habit of using execution bitmaps as ground truth.
3. Hand-written `exit_mx_at`. **Rejected so far**: it is an unchecked
   assertion, and asserting a width the ROM does not exit in miscompiles
   silently — the same failure class as the inline-argument bug.

### A2. `brk_at_*` is probably *correct* — confirm and close

67 nodes, 638 instructions. Measured: **65 of the 67 have an executed entry
point, but 0 of the 67 BRK sites ever executed.** That is exactly the
signature of a genuine width refutation — real code, entered correctly,
decoded at a width that eventually lands on a `$00` the CPU never reaches.

So this is the poison working as designed, not a coverage bug. Worth one pass
to confirm the two nodes with unexecuted entries, then stop treating this as
a blocker.

### A3. The last 96 executed-but-unanalyzed addresses

Down from 228. Remaining regions:

| region | size | note |
|---|---|---|
| `01:8988-8A3C` | 181 b | continuation past `01:8985`'s `JSR ($88ef,X)`; blocked by A1, not by table recovery |
| `01:AD12-AD2D` | 28 b | |
| `01:ACF4-AD03` | 16 b | |
| `01:8E37-8E3C` | 6 b | |
| `01:946B-946F` | 5 b | |
| `00:805F`, `00:80B1` | 1 b each | single addresses; likely interrupt-path fragments |

Most of this falls out of A1 for free.

### A4. Upstream items already written up, not yet filed

`docs/UPSTREAM_cop_syscall.md` and `docs/UPSTREAM_inline_args.md` are
filing-ready. Still open:

- The COP fix itself (implemented here, verified, not offered upstream yet).
- AOT vs interpreter **cycle-count disagreement**: 13 of 64 trials, always
  with the compiled side counting more. Logic matches every time. Matters for
  any host advancing devices off `cpu->master_cycles`.
- MSVC has no `__attribute__((weak))`, so `cpu_state.c`'s fallback guard table
  is a strong definition and collides. Worked around with `/FORCE:MULTIPLE`;
  `__declspec(selectany)` is the real fix.
- Close issue #14 and post the PR-17 reply (`docs/upstream/reply-pr17.md`).

### A5. Repo integrity: the submodule pointer is local-only

The submodule remote is `mstan/snesrecomp` (upstream), but `master` now
points at three commits that exist only on this machine — `61df24b`
(inline args), `88d05b8` (joypad revert), `d4aaf40` (COP). **A fresh
`git clone --recurse-submodules` cannot check out the submodule.** Either
push a fork and repoint `.gitmodules`, or carry the three as patch files in
this repo. This affects anyone trying to reproduce the results.

---

## B. Static recompilation

### B1. Step 3d — drive the guest inside the fiber

The fiber layer and the frame-boundary HLE (`hle_func 930d`) exist and
self-test. What is missing is an interpreter-with-bouncing driver so the
guest actually runs inside the fiber. See `docs/MIGRATION_step3.md`.

### B2. Is the AOT tier actually faster here?

Never measured. Both tiers are byte-identical on seven save states, so a
straight wall-clock comparison over a fixed frame count is now cheap and
would say whether continuing to push coverage is worth anything at runtime,
or whether the interpreter is already fast enough on a modern host.

---

## C. Reverse engineering

### C1. How is the tick routine entered? (narrowed)

`03:8000` is declared as a root because nothing names it: no `JSR $8000`,
no `JSL $038000`, no `JMP $8000` anywhere in the ROM, no executed indirect
dispatch table containing it, and no `PEA $7FFF`/RTS trick.

Narrowed since: tracing `03:8026` (the `INC $0b51`) **does** hit, arriving
from `03:84c3`, while tracing `03:8000` in the same run does not. So the
routine body runs every tick but entry at `$8000` is rare or one-time —
`$8000` is a prologue (`SEP`/`REP`/set DB=3) that the per-tick path skips.
Finding the real per-tick entry needs a longer PC history than
`SC_PC_HISTORY_SIZE` currently keeps.

### C2. The `$c5` reason-code dispatch — 12 handlers, semantics unknown

`01:897f` is `LDA $c5 ; REP #$10 ; ASL A ; TAX ; JSR ($88ef,X)`. The table is
12 word entries at `01:88ef`, bounded by `01:8907` being its own node:

```
[0] 8D25  [1] 8D26  [2] 8DCE  [3] 8E28  [4] 8E3D  [5] 9D6B
[6] 9F2D  [7] C529  [8] 93A8  [9] 93A4  [10] 940F  [11] 94E6
```

Only reason 1 (`8D26`, cursor sprite + direction dispatch) is understood.
`tools/dis_mx.py` now makes these readable.

### C3. The `$01df` UI sub-state machine — 5 states

Two parallel tables at `01:9d1a` (call) and `01:9d3a` (jump), handlers
`01:A886 / A97C / AA39 / AAD5 / AD54`. Same cycle as A1. Identifying what the
five states *are* would also make an `exit_mx_at` assertion verifiable by
inspection.

### C4. Tick / calendar / seasons / population / budget — **done**

Written up in `docs/ROM_MAP.md`. Time base (200 frames per tick, 4 ticks per
month), the season tables at `03:8160`/`03:816d`, the population formula
`(($0b8f + $0b93) * 8 + $0b8b) * 20`, and the annual budget at `03:8df1`
with the treasury clamped to 999,999.

Both follow-ups it raised are now closed. The zone tallies are
`$0b8b` = residential, `$0b93` = commercial, `$0b8f` = industrial, settled by
the class bases in the three helpers (153 / 324 / 513) and confirmed by their
relative magnitudes in a real city. `$0b1d` is the bank loan, confirmed from
play, charging 500 a year.

Gift-building income is also mapped: `03:ae61` pays 300/year for tile `$02fe`,
200 for `$02ec`, and 100 for each of `$02f5`, `$0319`, `$032b`, `$0334`,
`$033d`, `$034f`, accumulating into `$0ddd` -> `$0dd9`.

**Still open: which building is which tile index.** Eight tile IDs pay out and
none is identified. Placing each gift in play and watching `$0c71` and `$0ddd`
would name them in one session — the casino is known to be one of the six
100/year tiles.

### C5. Disasters — the `$0199` model was WRONG, and the mechanism is unfound

An earlier revision of this section claimed `03:b84b` / `$0199` was the
disaster machinery: a pending-disaster bitfield with handlers at `03:b9db`
and `03:ba47`. **That is disconfirmed.** Recording a session in which fire,
flood, tornado, earthquake and the monster were all set off:

- the dispatch body `03:b871-b8a1` still shows **1 of 48 bytes executed**,
  unchanged from before the session;
- `$0199` is **0 in every one of the six disaster save states**;
- only **13 addresses in the whole ROM** executed that never had before.

Two independent disconfirmations. Whatever `$0199` gates, it is not the
disasters. The static reading of the ladder was correct as far as it went —
it really is a three-bit field with per-bit handlers — but the inference that
it meant "disasters" came from the `$0425` No-Disasters cheat being tested a
few instructions earlier, which is proximity, not evidence.

The 13 newly executed addresses are the only real lead:

```
00:AD10 00:AD11 00:AD14 00:AD15
00:B18E 00:B190 00:B192 00:B193 00:B194
03:BAA6   03:E21E 03:E221 03:E224
```

`03:BAA6` does fall inside `03:ba47`, so that routine may be disaster-
adjacent after all, but one byte is not a finding.

### Why the six save states could not be replayed

All six sit in **`$01df = 2`** (the Boston Meltdown one in `$01df = 4`), a
bank-01 UI mode in which the simulation does not tick at all. Over 600 frames
of replay, a frozen state executes 1,315 addresses in bank 01 and 667 in bank
03 and never reaches `03:8026`; a state that runs executes 558 in bank 01 and
3,834 in bank 03. Bank 02 is untouched entirely while frozen.

Pressing B, Start, A, Select and X for six frames each changes nothing —
`$01df` stays 2 and the tick counter never moves. So these states cannot be
driven forward headless without knowing what input that mode expects.

### What would actually settle it

The coverage bitmap records every executed address, so **no save state is
needed** — the disasters simply have to *play out* during a recorded session
rather than being set up and then reloaded past. The session log for the
attempt above shows saves and loads interleaved and ending on loads, which is
consistent with each disaster being armed and then abandoned.

So: start a session, trigger one disaster, **let it burn for a while without
reloading**, then the next. A save taken *during* a visible disaster would
also work and would additionally allow a headless replay, provided it is
taken in normal play mode rather than `$01df = 2`.

### C6. Smaller open threads

- Moving-object entity identities (the sprite/vehicle table).
- What `03:9035`'s 6-slot window smooths.
- Bank-01 UI block layout generally.

---

## D. Game features

### D1. Sylt hack as a 9th scenario entry after Las Vegas

Nice-to-have, tracked as task #1. The map format is fully solved and verified
byte-exact (`docs/REFERENCE_map_format.md`), so this is mostly scenario-table
plumbing rather than new research.

### D2. Let a won scenario keep playing — no win check, no ending

Requested. Once a scenario has been beaten, allow carrying on in it freely
instead of being evaluated and ended.

The ROM already implements exactly that state for one index. The win/lose
evaluator at `03:c548` **deliberately returns without writing a result for
index 7** — free play — which carries the sentinel deadline `$ffff` in the
8-entry table at `$03c5b3`. So this is not new behaviour to invent; it is an
existing path to route a won scenario onto.

Two ways in, both fitting machinery that already exists:

- **Host-side**, alongside `SCENARIO OVR` and `UNLOCK SCENARIOS` in
  `src/main.c`: when the scenario's completion bit is set in `$700007` (bit N
  per the mask table at `03:e334`, read into `$42` at init), make `03:c548`
  take its index-7 path.
- **Deadline substitution**: give the scenario the `$ffff` sentinel that
  index 7 carries, so the timer never expires.

The first is more honest about intent and easier to toggle; the second is a
smaller change but conflates "won" with "no time limit", which are not the
same thing if the ending is triggered from somewhere other than the deadline.
Worth checking which of the two actually gates the ending before choosing.

---

## E. Verification gaps

- Coverage is the union of four recorded sessions. Rare paths — the end
  screen, disaster handling, some helper dialogs — may still be unrepresented,
  which would understate both the frontier and the executed-code percentages.
- The seven-save-state differential runs 410 frames with one scripted input
  pattern. A longer randomised-input differential would be stronger evidence
  that the COP widening is behaviour-preserving on paths not yet exercised.

---

## F. Measuring exit M/X instead of proving it

A1 says the remaining ~5% is the exit-M/X fixpoint, and that hand-written
`exit_mx_at` was rejected as an unchecked assertion. The measurement half of
the "measure and check" option now exists.

`SC_MX_BITMAP=<path>` records the executed-PC bitmap split four ways by the
live `(m,x)` flags — 4 x 64 x 4096 bytes, index `m<<1|x`. A call's return
address executes in exactly the widths its callee exited with, so this turns
the proof obligation into an observation.

`tools/mx_exit_report.py` reads that plus the manifest's
`unproven_call_at_..._to_...` reasons and reports, per unproven callee, the
widths seen at its callers' return addresses.

Over a union of 15 runs (a boot plus every preserved save state, 1,200-1,500
frames each):

```
00:8061   2 sites   m0x0            single
00:C3F9   8 sites   m0x0 m0x1       SPLIT
01:8907   2 sites   m0x0            single
02:A3DC   2 sites   m0x0            single
02:A3E0   5 sites   m0x0            single
03:8DF1   1 site    m0x0            single

5 single, 1 split, 7 never observed returning
```

Two things worth taking from this.

**`03:8DF1` is the one that blocks `03:8000`**, the monthly simulation tick —
the worked example in A1 of a node that is `aot_eligible` with an empty
`reasons` list and still truncates its caller. It is observed exiting `m0x0`
and nothing else.

**`00:C3F9` exits in two different widths.** That is not a gap in the
measurement, it is a fact about the routine, and it means no single
`exit_mx_at` line could ever be correct for it. Had the hand-written approach
been taken on the strength of "it looks like it always returns m0x0", this is
the one that would have miscompiled silently.

### What is deliberately not done

The report emits no cfg. A single observed width is *consistent with* a single
exit width, not proof of one — an unexercised path may exit differently, and
15 runs of ordinary play is not a proof of totality. Turning this into
`exit_mx_at` lines requires the analyzer to **verify** them rather than trust
them, which is the "check" half and is upstream work.

### F1. Cross-checking published exits against measurement — not sound yet

The obvious next use of the M/X bitmap is to validate the analyzer: for every
direct call whose callee *has* a published exit mode, the widths observed at
the return address should be inside that published set. Run over 2,235
distinct (site, callee) pairs, 40 come out as mismatches.

**Those 40 are not analyzer bugs, and the check as written cannot show that
they are.** At least two confounds are already visible in the output:

- **Inline arguments.** The check reads the return address as `site + 3` for
  `JSR` and `site + 4` for `JSL`. Five routines in this ROM consume bytes
  after the call and resume past them, across 125 call sites — for those the
  real return is `site + len + skip`, so the measurement is taken at an
  operand byte. `00:824B -> 00824F` is exactly this shape: a three-byte `JSR`
  whose target sits four bytes on.
- **Reachability.** A return address is not reached *only* by returning. If
  the same address is also a branch target or a fall-through, the observed
  width set is a union over all the ways in, not the callee's exit.

Both are fixable — skip the known inline-argument callees, and exclude return
addresses with any other predecessor — but until they are, this comparison
cannot accuse the analyzer of anything. Recorded because the raw number is
tempting: "40 mismatches" reads like a finding, and publishing it as one would
be the same mistake as the `$0199` disaster attribution, where a plausible
correlation got written up before it was tested.

What the run *does* establish is the shape of the data: 2,235 pairs have both
a published exit and an observed return, so once the confounds are handled
there is enough measurement here to check the solver properly.

### Both confounds handled — and the residue is systematic, not 26 bugs

Correcting the return address for the five inline-argument callees (168 sites)
and excluding every return address that any executed branch or jump can also
land on (144 sites) leaves:

```
checked 518   agree 492   mismatch 26
```

**Every one of the 26 differs only in the X bit, always the same way** —
published `x=1` where the machine shows `x=0`:

```
00:8130 -> 00C1FA:M0X1   observed m0x0   published m0x1
00:8432 -> 008436:M1X1   observed m0x0 m1x0   published m0x1 m1x0
02:A64D -> 02A651:M1X1   observed m1x0   published m1x1
03:94A0 -> 03A29A:M1X1   observed m1x0   published m1x1
```

Twenty-six independent solver bugs would not all land on the same flag in the
same direction. That is the signature of a convention mismatch on one side.

**Tested, and the host is right.** `SEP #$10` sets the X flag (8-bit index)
and `REP #$10` clears it (16-bit), so the width at the instruction *after* one
of those is ground truth. Over the executed sites:

| after | X must be | host bitmap records |
|---|---|---|
| `SEP #$10`, 47 sites | 1 | **x=1 in 11 observations, x=0 in 0** |
| `REP #$10`, 267 sites | 0 | **x=0 in 92 observations, x=1 in 0** |

Zero contradictions in either direction, so `cpu->xf` is recorded with the
standard sense and the measurement is not the problem.

That leaves the disagreement on the analyzer's side, with one alternative not
yet excluded: the check looks up the callee variant named by the *demand's*
target `(m,x)`, so if a site actually reaches a different variant at runtime
than the demand records, the published exit being compared is the wrong one.
Distinguishing "the published exit X is wrong" from "the demand names the
wrong variant" needs the callee's entry width measured too — which the same
bitmap can supply, by reading the width at the target address rather than at
the return address.

The 492 agreements are worth noting on their own: where the two do agree, they
agree exactly, across 492 call sites and both flags. That is real evidence the
solver is sound where it publishes at all — which was never in doubt but had
never been measured.
