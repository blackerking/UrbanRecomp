# What is left for investigation

State as of the measured-exit-M/X work (AOT share **94.93%** analyzed, 1,475
of 1,584 variants). Ordered by value, not by area. Each item says what is
actually known, so the next session does not re-derive it.

---

## A. Recompiler coverage

### A1. The exit-M/X fixpoint — the whole remaining 5%

**Partly closed.** The worked example below is fixed; the bank-01 cycle is
not, and is now blocked by *coverage*, not by method. See §F for the
measurement machinery and §F2 for what remains.

Current state: 1,584 nodes, 1,475 AOT-eligible, 109 LLE-only, **94.93%** of
analyzed instructions (was 94.83%). 107 nodes still publish no exit M/X, and
an unpublished exit truncates *every* caller, transitively.

> The "300 of 1,584" figure this section used to carry was stale — it predates
> the inline-argument fix, which took LLE-only from 348 to 110 on its own.

It is not a decode or table-recovery problem. `038DF1:M0X0` was `aot_eligible`
with an **empty `reasons` list** — clean, compiled — while its caller
`03:8000` was still `lle_only` with `truncated_call_continuation`, purely
because `038DF1` had no entry in the manifest's `exit_modes`. Compilability
and exit-width publication are separate fixpoints.

**`03:8000` is now AOT-eligible.** `exit_mx_at 038df1 0 0` in
`recomp/bank03.cfg`, measured rather than asserted (§F), unblocked the
monthly simulation tick — the one node this section named as the example.

The remaining LLE-only set splits cleanly:

| | nodes | instructions |
|---|---|---|
| genuine width refutation (`brk_at_*` / `structural_poison`, = A2) | 67 | 715 |
| blocked only by an unproven callee exit | 42 | 2,727 |

So the ceiling for this line of work is **99%**, and the last 1% is A2, which
is the poison working as designed rather than a bug.

The shape that defeats the solver is a mutually recursive dispatch cycle.
Bank 01's UI state machine is the clean example: `$01df` selects through two
parallel tables at `01:9d1a` (call) and `01:9d3a` (jump), and the five
handlers `01:A886 / A97C / AA39 / AAD5 / AD54` each dispatch through them to
all five. Each is unproven because the other four are.

Options, best first:

1. An SCC fixed-point solver upstream. `v2_analyze.py` already has an
   `assumptions` set and a comment about a "closed SCC solver", so this was
   anticipated and not finished. **Still the right fix for the bank-01
   cycle**, because measurement cannot reach it without play coverage.
2. **Measure and check.** Record exit M/X per callee from a real run, then
   emit `exit_mx_at` lines *and have the analyzer verify them* against the
   recording. **Done — see §F.** `tools/mx_exit_propose.py` emits them,
   `tools/mx_exit_check.py` verifies, `tools/gen_align_check.py` catches the
   silent-miscompile failure mode directly in the emitted C.
3. Hand-written `exit_mx_at`. **Still rejected**: it is an unchecked
   assertion, and asserting a width the ROM does not exit in miscompiles
   silently — the same failure class as the inline-argument bug. Every
   directive now in the cfgs came from measurement and is re-checked after
   regeneration.

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

> **This paragraph is unsafe and should be re-tested before anything is built
> on it.** "Pressing B, Start, A, Select and X" was almost certainly done with
> `--input` masks taken from the `$4218`/`$4219` word layout, in which
> B/Y/Select/Start are bits 12–15 and are **not button bits at all** — the
> runner takes serial order, `B=$0001 … R=$0800` (mstan/snesrecomp#17, and see
> §F3). Under the wrong masks those four presses were no-ops, so the
> experiment could not have moved `$01df` whatever the game does.
>
> Re-measured with correct masks at frame 400: `$01df` is **3**, not 2, in
> savestates 1, 2, 7 and 9, and the tick counter `$0b51` is advancing in all
> four. On `savestate_9` any button press moves `$01df` 3 → 1. So the states
> are neither frozen nor input-deaf, and "these states cannot be driven
> forward headless" does not survive.
>
> The disaster hunt should be retried on that basis before falling back to
> "let it burn during a recorded session" below.

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

> **Resolved: there were no mismatches.** It was a third confound in the
> check, not a disagreement. Skip to "The 26 explained" below; the two
> paragraphs after this one are kept because the SEP/REP calibration they
> report is still the thing that ruled out the measurement side.

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

### The 26 explained — it was the check, and the analyzer is sound

That last alternative is exactly what it was. **The mismatches are an artefact
of comparing against a variant that never runs**, and the fix needed no new
instrumentation.

A call site does not carry one demand, it carries a demand *per variant*. The
analyzer cannot prove the entry width statically, so it emits both, and for a
callee that leaves X alone it correctly publishes exit `x=1` for the `x=1`
variant and `x=0` for the `x=0` one. Both rows are right. Comparing the
machine against whichever row the enumeration reached first then accuses the
solver of disagreeing with a variant the ROM never enters.

The siblings make it obvious once they are printed together:

```
008130 -> 00C1FA:M0X0  published m0x0      <- the variant the machine enters
008130 -> 00C1FA:M0X1  published m0x1      <- the one the check was reading
0394A0 -> 03A29A:M1X0  published m1x0
0394A0 -> 03A29A:M1X1  published m1x1
```

Observed at `00:8130`: entry `m0x0`, exit `m0x0`. The `M0X0` row matches
exactly; the `M0X1` row is dead.

**Keying on the measured entry width fixes it, and the key is free**: `JSR`
and `JSL` change neither M nor X, so *the width recorded at the call site's
own address is that site's entry variant*. The callee's target address is the
wrong place to read it — that unions every caller.

`tools/mx_exit_check.py` does this, and corrects four confounds in total:
inline-argument return addresses, return addresses any executed branch/jump
can also land on, call sites that never executed, and the variant key above.

```
checked 473   agree 473   mismatch 0
```

Two guards against this being a vacuous result, because keying the lookup on
the entry width would trivially agree for any callee that just preserves M
and X:

- **150 of the 473 publish an exit width that differs from the entry width**,
  in both directions on both flags — `m0x0 -> m1x0` 64 times, `m1x0 -> m0x0`
  48, plus X changes. Those are the rows with something to be wrong about,
  and they are all correct.
- **Fault injection**: flipping 40 random published exits in the manifest
  makes the check report mismatches; the true manifest reports none.

So the solver is sound everywhere it publishes, measured across 473 call
sites and both flags. The earlier "26 systematic" reading was right to be
suspicious of itself and right not to be filed upstream — there was no bug to
file.

### F2. Directives emitted, and why only three

`tools/mx_exit_propose.py` turns the measurement into cfg directives. It reads
the exit width **at the callee's own `RTS`/`RTL`**, not at its callers' return
addresses, because 44 of the 60 unproven call sites are `JSR (abs,X)` — fixed
return address, runtime-chosen target — so the widths seen at the return are a
union over whichever handler the dispatch picked. The width at an `RTS` is the
exit width of the routine that instruction belongs to, by definition.

It proposes a callee only when all of the following hold, and says which test
each rejection failed:

- every executed return point in the body was measured, not just one;
- they all agree on a single width;
- the return-address reading, where a direct call exists, does not contradict
  it.

Three passed, and all three were derived independently from two **disjoint**
halves of the recording, which produced identical directives:

```
exit_mx_at 01940f 1 0      recomp/bank01.cfg
exit_mx_at 038df1 0 0      recomp/bank03.cfg   <- unblocked 03:8000
exit_mx_at 03a89f 0 0      recomp/bank03.cfg
```

The rejections are the useful part:

| callee | why not |
|---|---|
| `00:C3F9` | genuinely split, `m0x0` **and** `m0x1` — no single `exit_mx_at` can be right |
| `02:A3E0` | split at the `RTS`, `m0x0`/`m1x0` — **the return-address reading alone said `m0x0` and would have proposed it** |
| `02:8000` | four executed `RTL`s, one measured; the other three could differ |
| 13 others | never observed returning — no evidence either way |

`02:A3E0` is the concrete argument for reading the `RTS`: the weaker method
was one step from emitting a wrong directive, and the stronger one caught it.

### F3. The bank-01 cycle is now a coverage problem, not a solver problem

The five handlers `01:A886 / A97C / AA39 / AAD5 / AD54` are the big remaining
prize — 12 nodes and 766 instructions each. They are **not** unmeasurable in
principle: `coverage_union.bin`, recorded from real interactive play, shows
every one of them executing, with their `RTS` instructions executing too.

> **Corrected.** The first version of this section concluded that only an
> interactive session could reach them, because 18 headless runs never did.
> That was an artefact of *the runs*, not of headless replay: the scripted
> input was written with the `$4218`/`$4219` word layout, in which
> B/Y/Select/Start are bits 12–15 — **not button bits at all** — so those four
> were never actually pressed and the UI state machine never advanced. The
> runner's `input*_currentState` is the *serial* order, `B=$0001 … R=$0800`
> (mstan/snesrecomp#17). Re-running with the correct masks reached
> `01:A97C` immediately.

With correct masks, headless replay drives the UI fine: input moves `$01df`
(on `savestate_9`, 3 → 1 on any button), and distinct PCs went 11,352 →
14,702. Two more callees became measurable and are now committed, each
derived independently from two disjoint halves of the recording:

```
exit_mx_at 019d6b 0 0       # $c5 reason-code handler 5
exit_mx_at 01a97c 0 0       # $01df UI handler
```

**They did not move the AOT share, and that is the SCC being an SCC.** The 12
nodes those handlers block also call `A886`, `AA39`, `AAD5` and `AD54`, so
publishing one of five changes nothing until all five land. The unproven set
went 18 → 16; coverage stayed at 94.93%. It is all-or-nothing by construction.

So the remaining work is coverage of the other four handlers, and it is no
longer known to need a human at the controller — targeted input from a state
that reaches each `$01df` mode should do it. What `01:A97C` cost was one
correctly-masked run, not a play session.

Option 1 in A1 — the upstream SCC solver — remains the route that does not
depend on reaching every handler at all, and given that four-of-five buys
nothing, it is looking like the better investment.

### F4. A decode desynchronisation found in the emitted C, and fixed

`tools/gen_align_check.py` is the third check, and the one that would actually
catch a wrong `exit_mx_at`. The AOT differential cannot: this host still runs
everything on the interpreter (`src/main.c`, "this host bypasses
common_rtl.c entirely"), so `SimCitySNESRecompAOT` links the generated banks
without executing them, and byte-identical WRAM between the two builds says
nothing about whether the emitted C is right.

The execution bitmap can. Decoding every executed address at the width it was
executed in yields the set of bytes that are *definitely operands*; an emitted
block label sitting on one of those is a decode desynchronisation, and no
reading of the listing makes it benign. It is one-sided on purpose — a label
at an address the bitmap never recorded proves nothing.

Over 15,032 emitted labels it found exactly one, and it was **pre-existing**,
not introduced by the directives above (the same check on a regeneration with
the directives removed reports the identical label):

```
00:926D  func with no entry_mx_at  ->  defaults to M1X1
00:926D  A0 00   decoded M1X1 as `LDY #$00`, two bytes
00:926F  80 48   so decode resumed here and read `BRA $92b9`
00:92B9          which is the middle of `DEC $012b,X` at 92b7
```

The routine is only ever entered with `x=0`: all ten call sites demand
`M1X0`, and the bitmap records only `m1x0` at `926d`, which makes the real
instruction `LDY #$0000` at three bytes with the next instruction at `9270`.
The `M1X1` variant was dead, so nothing miscompiled at runtime — but it was
`aot_eligible` with an empty `reasons` list, and would have become live the
moment anything routed to it.

Fixed with `entry_mx_at 926d 1 0` in `recomp/bank00.cfg`. The variant is gone
and the check is clean. Note the ordering requirement: `cfg_loader` applies
`entry_mx_at` at the point the `func` is parsed, in a single pass, so the
directive must appear **before** the `func` line.

This is worth generalising upstream. A cfg `func` silently defaulting to
`M1X1` is a trap whenever the routine is only entered at another width, and
the resulting body looks completely clean — same signature as the
inline-argument bug, and found the same way.
