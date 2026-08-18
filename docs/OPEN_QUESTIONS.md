# What is left for investigation

State as of the measured-exit-M/X work: **1,544 of 1,628 variants**
AOT-eligible, **99.3% of all executed code**. Ordered by value, not by area.
Each item says what is actually known, so the next session does not
re-derive it.

---

## A. Recompiler coverage

### A1. The exit-M/X fixpoint — the whole remaining 5%

**Closed for the cycle that motivated it.** All five bank-01 UI handlers now
publish measured exits, and the SCC they formed is broken. See §F for the
machinery and §F5 for the bounding bug that had been hiding half the
evidence.

Current state: **1,628 nodes, 1,544 AOT-eligible, 84 LLE-only.**

| | at the start of this work | now |
|---|---|---|
| AOT-eligible variants | 1,475 | **1,544** |
| LLE-only | 109 | **84** |
| AOT instructions | 64,386 | **73,364** |
| **executed code inside an AOT node** | 96.5% | **99.3%** |

**The analyzed share fell because the denominator moved.** Publishing an exit
lets decode continue past a call that used to truncate, so the frontier grew
from 67,828 to 74,576 instructions and edges from 4,518 to 6,632. The same
thing happened during the COP work, where the frontier grew by 27,000. Judge
this by absolute AOT instructions and by executed-code share, both of which
rose; the percentage-of-analyzed is a ratio with a moving bottom.

An unpublished exit still truncates *every* caller, transitively.

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

The 99% ceiling this section used to predict has been reached, measured
against executed code. What remains is A2's poison plus three callees
(§F11), and one of those is blocked only by a variant the machine never
enters.

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

### A2. `brk_at_*` is correct — **CLOSED**

67 nodes, 715 instructions. Re-measured against the full coverage union
(32,107 executed addresses), and the picture is stronger than the earlier
reading:

| | earlier | now |
|---|---|---|
| poisoned nodes whose entry point executed | 65 of 67 | **67 of 67** |
| BRK sites ever executed | 0 of 67 | **0 of 71** |

The two nodes with unexecuted entries are covered now, so every poisoned
node is real code that the CPU genuinely enters, and not one of the BRK
sites the analyzer decoded is ever reached.

**That much is only *consistent* with the poison being right — an
unexercised path would look the same.** The positive proof comes from the
operand-byte set `gen_align_check` already builds: decode every executed
address at the width it was executed in, and the interior bytes are
definitely operands.

```
brk sites proven to be operand bytes of a real executed instruction: 60 of 71
brk sites with no such proof (never-executed region)               : 11
```

So for 60 of the 71 sites the `$00` the analyzer tripped over is
**demonstrably the middle of an instruction the CPU really ran** — e.g.
`00:C74A` is an operand byte of the instruction at `00:C748`. That is a
misaligned decode, which is exactly what the poison exists to reject. The
remaining 11 lie in regions with no measurement, so they are unproven in
either direction rather than suspicious.

**Stop treating this as a blocker.** The 715 instructions are the
irreducible remainder of the static approach, not a coverage gap.

### A3. Executed-but-unanalyzed addresses — **CLOSED, zero remain**

228 -> 96 -> **0**. Every one of the 32,107 addresses the game is known to
execute now falls inside an analyzed node.

It did fall out of A1 for free, as predicted: the regions listed here were
continuations past calls with unproven exits (`01:8988-8A3C` past
`01:8985`'s `JSR ($88ef,X)`, `01:ACF4-AD03` in the View path), and
publishing those exits pulled them into the frontier.

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

**Much reduced.** Upstream merged both of the PRs this repo was carrying
local versions of — #17 (auto-joypad byte order) and #19 (pop/push inline
arguments) are in `origin/main` as of `9d6ad3c`. Our local `b48daf4`/`88d05b8`
and `61df24b`/`cebda0b` were add-then-revert pairs that cancelled to nothing,
so rebasing onto upstream lost no work: the regeneration is identical
(1,625 variants, 1,532 AOT-eligible, 6,705 edges) and all eleven save states
remain byte-identical between tiers.

The submodule now sits on `simcity-main` = `origin/main` + **two** commits,
both genuinely ours and both upstreamable:

| commit | what | status |
|---|---|---|
| `0183d9a` | Model COP as a tier-to-LLE call instead of structural poison | write-up ready in `docs/UPSTREAM_cop_syscall.md`, never offered |
| `3dbd292` | `cfg: add exit_mx_set` | PR-ready on branch `pr-exit-mx-set` (`origin/main` + one commit, 56 Rust + 81 Python tests pass) |

**Still true: a fresh `git clone --recurse-submodules` cannot check these out**,
because neither commit is pushed anywhere. But the fix is now two PRs rather
than a fork-and-repoint, and if both land the submodule can point at plain
upstream.

Moving to upstream `main` needed one integration fix, carried in this repo's
`CMakeLists.txt`: upstream added `runner/src/snes/tier2_capture.c`, which
`interp_bridge.c` now calls into, so every target compiling `interp_bridge.c`
needs it too (three of them here). Without it the AOT target fails to link on
`tier2_capture_manifest_path` / `tier2_capture_append_discovery`.

---

## B. Static recompilation

### B1. Step 3d — the guest runs inside the fiber, and deadlocks

**Attempted; the driver exists and the diagnosis is now specific.**
`SC_FIBER=1` on the AOT build creates the game fiber, installs the vblank
yield, and enters the guest. `src/simcity_fiberdrive.c`, strictly opt-in;
the default path is verified byte-identical between tiers with the fiber
code linked but inert.

Two things changed the picture:

- **A compiled entry point exists now.** §5 of `MIGRATION_step3.md` ruled out
  ar-recomp's design because both architectural entries were `lle_only`.
  `03:d283` is compiled, so `008000:M1X1` is too, and the dispatch table
  carries it as `I_RESET_M1X1`. Measured: the fiber switch works and the
  compiled reset handler is entered.
- **It then hangs, for a reason the entry point was masking.** Devices only
  advance while the *host* holds the fiber, so any guest loop spinning on a
  hardware status register deadlocks. The boot path has one, and it executes
  in every recorded session: `00:9280 LDA $4212 ; AND #$01 ; BNE $9280`,
  waiting on auto-joypad-busy, which clears only as the beam advances.

Worse, a bare call to a compiled body has **no execution bound at all**:
`interp_bridge_lle_master_deadline_reached`, which every generated block
polls, requires `s_lle_sched_depth > 0 && s_interp_bounce_owner_depth > 0`
and is inert outside `interp_bridge_run_scheduler`. So the host cannot even
time the guest out.

Next step is a choice between HLE-ing the status spins, yielding on device
reads, or driving through `interp_bridge_run_scheduler` — laid out with
trade-offs in `docs/MIGRATION_step3.md` §7. The third is the framework's own
direction and the only one that restores the bound.

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

**Practice/free play is a second existing model for this, and a closer one.**
Reported from play: the practice map *does* run a win check against
population, and you can carry on playing after it fires. So "evaluated, and
then not ended" is not hypothetical behaviour that has to be invented — the
ROM already does it in the mode people spend the most time in. Worth reading
how practice differs at `03:c548` before choosing between the two routes
below, because it may already be the exact path a won scenario should take.

Related, and now mapped: the 50,000-population milestone in that mode fires
**message 12**, which is what unlocks View (`$01e7` bit 1, docs/ROM_MAP.md).
That is the one confirmed case of a population threshold driving a message
rather than a state change, and the win check is likely to be built the same
way.

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
| `02:A3E0` | ~~split at the `RTS`~~ — **retracted, see below** |
| `02:8000` | four executed `RTL`s, one measured; the other three could differ |
| 13 others | never observed returning — no evidence either way |

> **The `02:A3E0` claim is withdrawn.** It read as the argument for preferring
> the `RTS` over the return address — the weaker method "one step from a wrong
> directive, and the stronger one caught it." That split was an artefact of how
> the tool bounded a routine's body, not a property of the routine. With the
> bounding fixed (§F5) `02:A3E0` has exactly one executed return, `m0x0`, which
> is precisely what the return-address reading said. The two methods agree, and
> `A3E0` is now emitted as a directive.
>
> Reading the `RTS` is still the right primary source, for the reason given
> above — 44 of the 60 call sites are `JSR (abs,X)`, so return-address widths
> union over whichever handler the dispatch picked. That argument stands on its
> own. It just never needed this example, and the example was wrong.

### F3. The bank-01 cycle — CLOSED

> All five handlers now publish measured exits and the SCC is broken (§F5).
> The section below is kept for the reasoning and for the correction it
> carries, but its conclusion — that this still needed doing — is superseded.

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

### F5. The bounding bug that was hiding half the evidence — and the close-out

`mx_exit_propose.py` bounded a routine's body by its node's
`min_pc24..max_pc24`. That is wrong in both directions, and it was quietly
rejecting routines whose evidence was actually complete:

- **Nesting.** `01:AAD5`'s node spans 541 bytes and physically contains the
  whole of `01:AC0E` and `01:AC23`. Their returns were counted as AAD5's, so
  it looked like a three-return routine with one measured and was rejected as
  PARTIAL. It has exactly one return, `01:AC0D`, and it was measured all along.
- **Truncation.** `01:AC0E`'s node stops at `AC19`, because the `JSR $ac23`
  there has an unproven exit. Its real return at `AC22` therefore lies outside
  every node's range and got attributed to whatever enclosed it.

Bounding a routine by **the next entry address** fixes both, and assumes only
that routines are laid out contiguously — which, if violated, surfaces as a
contradictory split rather than a wrong directive. The bug was conservative:
it over-counted return points, so it under-proposed and never mis-proposed.

With that fixed, plus two recorded play sessions and 65 correctly-masked
headless runs, eight directives became emittable — including **all five**
bank-01 handlers, every one exiting `m0x0`:

```
exit_mx_at 008061 0 0     exit_mx_at 01aad5 0 0     exit_mx_at 02a3dc 0 0
exit_mx_at 01a886 0 0     exit_mx_at 01ad54 0 0     exit_mx_at 02a3e0 0 0
exit_mx_at 01aa39 0 0     exit_mx_at 02a0e8 1 1
```

Verified after regeneration: **1,109 call sites checked, 1,109 agree, 0
mismatches** (435 of them publishing an exit that differs from the entry
width); 790/790 on a holdout half alone; `gen_align_check` clean over 16,039
emitted labels; 81 framework tests pass; and all eleven save states remain
byte-identical in 128 KB WRAM between the interpreter and AOT tiers.

Caveat worth keeping: `01ad54` and `02a0e8` are supported by the full union
only. Each disjoint half saw one of `01:AD54`'s two return points but not
both, so neither half alone clears the "every executed return measured" bar.
The other six are confirmed independently by both halves.

### What is left

93 LLE-only nodes: 715 instructions of `brk_at_*` poison (A2, working as
designed) and 6,683 instructions blocked by **five** callees still without a
publishable exit:

| callee | blocks | status |
|---|---|---|
| `02:8000` | 6 nodes / **5,388 instr** | its own return `02:8195` is executed but not yet measured — by far the biggest remaining prize |
| `00:C3F9` | 4 nodes / 740 instr | **proven split** (`m0x0` and `m0x1`), both returns measured |
| `01:AC0E` | 2 nodes / 44 instr | return `01:AC22` executed, not measured |
| `01:AC23` | 1 node / 5 instr | 2 returns executed, neither measured |
| `02:8196` | 1 node / 6 instr | 1 of 3 returns measured |

Four of the five are ordinary coverage gaps — the return points are known,
executed, and simply have not been caught in an M/X recording yet. `02:8000`
alone is worth more than everything this session added.

`00:C3F9` is the exception and needs something the cfg cannot currently
express: a **per-entry-variant** exit. `cfg_loader` has the
`exit_mx_at_per_variant` field and `v2_regen._rebuild_callee_exit_mx` consumes
it, but only `exit_mx_autoroute` populates it — there is no cfg directive. The
broadcast `exit_mx_at` cannot be used here, and the autoroute docstring is
explicit that broadcasting a variant-dependent exit "poisons non-default
callers". Adding a parser for it is a small, clearly-scoped upstream change,
and it is the natural next contribution to `mstan/snesrecomp` after #17.

### F7. `02:8000` is genuinely multi-exit — and what that costs

A recorded session that opened the map overlay windows reached it at last:
`01:AB41`'s `JSL $028000` runs, the whole 167-address body executes, and the
`RTL` at `02:8195` is measured in `m1x1`. That was supposed to be the last
big directive — 6 nodes and 5,388 instructions.

It is not emittable, and the tool refused it before anything was written:

```
CONFLICT: RTS/RTL says m1x1 but callers return in m0x1/m1x1
```

The conflict was the body bound. `body_of` assumed a routine is contiguous
from its entry to the next entry, and `02:8000` is not: **`02:803C` is
`JMP $824B`**, jumping into a shared tail that ends at `02:8374`, which
returns in `m0x1`. So the routine really does exit two ways, and the caller's
return-address widths were right all along. This is the contiguity assumption
failing exactly as the code comment predicted — "shows up as a contradictory
split rather than a wrong directive".

`body_of` now follows executed unconditional direct jumps (`JMP`, `JML`,
`BRA`) out of the extent, transitively. Conditional branches are deliberately
not followed: they stay inside a routine far more often than not, and chasing
them would merge unrelated code. An unfollowed edge costs a rejection, never a
wrong directive. With that, `02:8000` reads 4/4 returns measured and SPLIT.

**Both remaining large callees need something the cfg cannot express.**

| callee | blocks | exits observed |
|---|---|---|
| `02:8000` | 6 nodes / 5,388 instr | `m0x1` **and** `m1x1` from a single `m0x0` entry |
| `00:C3F9` | 4 nodes / 740 instr | `m0x0` **and** `m0x1` |

Note what `02:8000` is *not*: this is not a per-entry-variant split, which
`exit_mx_at_per_variant` would cover. One entry variant produces two exit
widths depending on the path taken. The manifest already has a representation
for that — `exit_mode_sets`, 184 of which the analyzer derives itself — but
there is **no cfg directive that can declare a set**, only the single-valued
`exit_mx_at`.

So the upstream ask is now specific and evidenced: a cfg directive that
declares a *set* of exit widths, feeding `callee_exit_mx_modes` the way
`exit_mx_at` feeds `callee_exit_mx`. That plus the per-variant parser covers
every remaining case in this ROM. Failing that, the SCC solver in A1 option 1
derives both without any directive at all, which is why it keeps looking like
the better investment.

Everything else is small: `01:AC0E` (2 nodes / 44 instr), `01:AC23` (1 node /
5 instr) and `02:8196` (1 node / 6 instr, also split on the same shared tail).

State at this point: **1,375 call sites checked, 1,375 agree, 0 mismatches**,
578 of them publishing an exit width that differs from the entry width.

### F8. `exit_mx_set` — the set-valued directive, implemented

F7 ended by naming the missing piece: a cfg directive that declares a *set*
of exit widths. It now exists, in the submodule on branch `feat-exit-mx-set`
(commit `31c0e8a`), in both the Rust analyzer and the Python path.

```
exit_mx_set <hex_addr24> <entry MmXn> <exit MmXn>[,<exit MmXn>...]
exit_mx_set 028000 M0X0 M0X1,M1X1      # recomp/bank02.cfg
exit_mx_set 00c3f9 M0X0 M0X0,M0X1      # recomp/bank00.cfg
```

The decoder needed nothing: `callee_exit_mx_modes` already forks the post-call
continuation once per exit mode and the emitter picks the live one with a
runtime width switch. Only the *declaration* path was missing. Three places
beyond the parser mattered, and the third is the interesting one:

- `declared_exit_sets` seeds `active_sets` rather than joining
  `declared_exit_modes`, because that map is single-valued and a set is a
  different kind of fact, not a stronger one.
- The round loop must not let an inferred exact fact replace a declared set.
- **The truncation sweep** drops derived exits for any node carrying
  `truncated_call_continuation`, guarded only by `declared_exit_modes`.
  Without a matching `declared_exit_sets` arm, a declared set is discarded for
  exactly the callees it exists to describe — the ones the solver truncated
  and therefore could never derive an exit for. `00:C3F9` is that case, and
  its directive was silently dropped until this was fixed. Worth remembering
  as a general shape: a new authoritative input has to be threaded through
  every place the old one was privileged, and the compiler cannot find them.

Rejects a single-element set, pointing at `exit_mx_at`, so it cannot be used
to smuggle in an exact fact under another name.

### F9. Where it actually got to, and the cascade

| | after F5 | now |
|---|---|---|
| AOT-eligible variants | 1,531 | **1,532** |
| AOT instructions | 67,178 | **67,182** |
| executed code inside an AOT node | 97.6% | 97.6% |
| edges | 6,632 | **6,705** |

`02:8000` published its set and decode ran on past it — which exposed
`01:AD04`, previously invisible, blocking 6 nodes and 5,460 instructions. It
measured cleanly (single return, `m1x1`, caller corroborates) and is now
declared. That in turn exposed more of `01:AC23`, whose demands went from 1 to
7.

**This is the pattern to expect from here: each unblocked callee reveals the
next.** The AOT instruction count barely moves while the frontier grows,
because the newly decoded code arrives already blocked by the callee behind
it. Nothing is wrong; it is just that the remaining work is a chain, not a
set, and the ceiling estimates in A1 assumed a fixed denominator.

Remaining, all small or structural:

| callee | blocks | why |
|---|---|---|
| `01:AC23` | 7 demands | 1 of 3 executed returns measured |
| `00:C3F9` | 4 demands | **all four cite the `m0x1` demand**, a variant the machine never enters — the bitmap has `00:C3F9` and its call site `00:813A` at `m0x0` only. Declaring `m0x1` would be an assertion about dead code, so it is deliberately not done; the real fix is upstream width tracking at the call site. |
| `01:AC0E` | 4 demands | return `01:AC22` executed, never measured |
| `02:8196` | 1 demand | genuinely split on the shared tail |

State: **1,388 call sites checked, 1,388 agree, 0 mismatches**, 584 publishing
an exit width that differs from the entry width; `gen_align_check` clean; 81
framework tests pass; eleven save states byte-identical between tiers.

### F10. The rest of bank 01 is the annual budget dialog

After `02:8000` and `01:AD04` landed, `01:AC23` became the head of the chain —
7 nodes, 5,663 instructions — needing two of its three returns measured.
Fifty randomised headless runs hit none of them. Reading the code instead of
hunting found the gate, the same way it did for `02:8000`:

```
01:AC94  LDA $0bcb
01:AC97  BNE $accf        <- always falls through in every recording
```

`$0bcb` is set to `$00ff` at `01:AB96`, which is itself behind:

```
01:AB89  LDA $0b35 ; BEQ $abc0
01:AB8E  LDA $0dc3 ; BEQ $abc0
01:AB93  LDA #$00ff ; STA $0bcb
```

**`$0dc3` is the budget-dialog busy flag** — already in `docs/ROM_MAP.md`:
`03:8EC8` sets it to 1 and then spins at `03:8ECE` until the UI clears it. So
the whole remaining bank-01 chain is the **annual budget dialog path**, and
`01:AC23` is the routine that services it.

That also explains the misses, and it is an embarrassing arithmetic error
rather than anything subtle: a year is 200 frames per tick x 4 ticks per month
x 12 months = **9,600 frames**, and the hunt runs were 2,600. They could not
have reached a year-end no matter how the buttons were pressed. Every
randomised hunt in F3/F5 was structurally incapable of finding this.

Re-running at 30,000 frames (three-plus in-game years) reaches it: `03:8EC8`
fires, the `03:8ECE` spin runs in every run, and `01:ACCF` and `01:AC22` are
measured for the first time.

Result: `01:AC0E` became fully measured and is declared; `01:AC23` went from
1/3 to 2/3 returns measured.

| | before | after |
|---|---|---|
| AOT-eligible variants | 1,532 | **1,535** |
| LLE-only | 93 | **91** |
| AOT instructions | 67,182 | **67,283** |
| executed code inside an AOT node | 97.6% | **97.7%** |

`mx_exit_propose.py` now separates **"SPLIT, fully measured"** from **"SPLIT
but INCOMPLETE"** and prints a paste-ready `exit_mx_set` line for the former.
The distinction matters more than it looks: a set missing one of a routine's
real exit widths is worse than no set at all, because the decoder forks the
post-call continuation once per declared width and an omitted width is a
continuation never decoded. "It splits" and "it splits and we have seen all of
it" are different claims, and only the second is safe to declare.

Under that rule `00:C3F9` (2/2) and `02:8196` (3/3) are declarable and
`01:AC23` (2/3) is not — it still needs `01:AD03`.

### Dense input is worse than sparse, measured

The obvious follow-up to "the runs were too short" was to also make the input
continuous — 60 presses each held ~450 frames, back to back across 27,000
frames, so no dialog window could be missed. It is **strictly worse**:

| run set (12 each, 30k frames) | `01:ACCF` | `01:AC22` | `01:AD03` | annual budget | `01:AC94` gate |
|---|---|---|---|---|---|
| sparse input (gaps of 300-600 frames) | 3/12 | 3/12 | 0/12 | 3/12 | **3/12** |
| dense input (held ~450, back to back) | 0/12 | 0/12 | 0/12 | 3/12 | **0/12** |

Both reach the annual budget equally often, because that is driven by elapsed
time rather than input. But continuous input reaches the `01:AC94` gate in
**none** of twelve runs against three of twelve for sparse: holding buttons
suppresses the very dialogs the coverage needs, presumably by dismissing or
blocking them as fast as they appear.

So the two knobs pull in opposite directions. Long runs are needed for
time-gated code; *idle gaps* are needed for UI-state code. Tuning one without
the other loses the thing you were hunting.

**The lesson worth keeping is about run length, not budgets.** Coverage
hunting had been tuned for *breadth* — many short runs with varied input —
when the missing code was gated on elapsed game time. Check what a path costs
in frames before concluding it is unreachable.

### F11. View mode closes the chain — 99.3% of executed code

`01:AC23` was the last big blocker, needing its third return measured. It was
not a coverage-luck problem: the branch that reaches it is `$01fb == 7`, the
**View mode** entry of the UI menu, and View is locked behind `$01e7` bit 1
(docs/ROM_MAP.md). Every automated hunt so far ran from save states where
View was **locked** — 0, 1, 2, 7, 8 all have `$01e7 = 0`. The path was
unreachable by construction, and roughly a hundred runs were spent on a closed
door.

Scenario states 3-6 carry `$01e7 = 0x0002` (View unlocked at init by
`03:C687`). One session that loaded such a state and opened View captured all
of it: `01:AAF6`, `01:AB0D`, `01:AB15`, `01:AC28`, `01:ACD0` and `01:AD03`.

`01:AC23` is genuinely two-exit from one entry variant — `01:ACCF` returns
`m0x1`, `01:AD03` returns `m0x0` — so it needs the set form:

```
exit_mx_set 01ac23 M0X0 M0X0,M0X1
```

| | before | after |
|---|---|---|
| AOT-eligible variants | 1,535 | **1,544** |
| LLE-only | 91 | **84** |
| AOT instructions | 67,283 | **73,364** |
| **executed code inside an AOT node** | 97.7% | **99.3%** |

Verified: 1,584 call sites checked, 1,584 agree, 0 mismatches (669 publishing
an exit width that differs from the entry width); `gen_align_check` clean; 81
framework tests pass; eleven save states byte-identical between tiers. The
session was checked in isolation first (360/360, alignment clean) before being
folded into the union.

**What actually unblocked this was game knowledge, not tooling.** The static
reading gave the gate (`$01e7` bit 1) but not its meaning; naming it as View
and knowing scenarios have it from the start is what turned an unreachable
branch into a ten-second capture. Worth remembering the next time automation
stalls: check whether the path is *possible* from the state being driven
before adding more runs.

### F6. `SC_FREEZE` runs must never enter an M/X measurement

Trying to reach `02:8000` headlessly, the gate turned out to be explicit:
`01:AB22` is `LDA $01fb ; CMP #$0002 ; BCC $ab38`, and the `JSL $028000` sits
on the taken branch. So freeze `$01fb` below 2 and the path should open.

It does not, for a mundane reason worth writing down: **`SC_FREEZE` is applied
once per emulated frame**, and the ROM writes `$01fb = 2` at `01:AC10` within
the same frame before the read. A frame-granular freeze cannot win a race
inside a frame. (`$01fb` is also a 16-bit read -- `REP #$20` two instructions
earlier -- so freezing only `$1fb` and not `$1fc` fails even on paper.)

The important part is what happened when those runs were folded into the
union anyway:

| union | mx_exit_check |
|---|---|
| 99 natural runs, 23,691 PCs | **1205 checked, 1205 agree, 0 mismatch** |
| + 20 `SC_FREEZE` runs | 1189 checked, 1187 agree, **2 mismatch** |
| the 20 frozen runs alone | 462 checked, 460 agree, **2 mismatch** |

`gen_align_check` independently flagged a desynchronised label in the frozen
union that is clean in every natural run, and the frozen runs recorded
executed PCs in **bank $18** -- outside the 512 KB ROM image, i.e. the CPU
running off into open bus.

Holding a byte at a value the ROM never holds there produces states the ROM
never reaches, and widths recorded in them are evidence about nothing. Freeze
remains a good instrument for "is byte X what gates behaviour Y"; it is not a
way to manufacture coverage. Both checks catching it independently is the
system working.

The 119-run frozen union has been deleted rather than kept; `mx_union.bin` is
the natural-runs union and is the one to extend.

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

### E1. Old save states restore with broken tiles — **CLOSED, stale files**

> Re-tested on a clean run: states load and render correctly ("works as
> intended, no problems"). The broken tiles were seen while the SDL3 texture
> blend bug was live, and did not survive its fix. Nothing to repair -- and
> had it been a genuine format incompatibility it would still have been a bug
> with no fix, since old blobs cannot be migrated. Kept for the reasoning
> below, which correctly ruled out both candidate causes.


Reported after the SDL3 black-screen fix: loading a pre-existing save state
renders the map with broken tiles, while **starting a new map renders
correctly**.

That "new map is fine" is what makes it diagnosable. The renderer, the tile
decoder and the PPU path are all evidently working — so this is a
**save-state restore** problem, not a rendering one, and not SDL-related at
all (SDL cannot affect guest VRAM).

The likely shape: the state blob captures WRAM and CPU/PPU registers, but the
tile data lives in VRAM, and either it is not captured, not restored, or is
restored without whatever re-upload the game normally performs on a screen
change. A new map runs the game's own tile upload and therefore looks right.

Worth checking in this order, cheapest first:

1. **Does the save/load path cover VRAM at all?** `snes_saveload` in the runner
   handles PPU state; confirm VRAM is inside that blob rather than assumed to
   be rebuilt.
2. **Do the existing states predate a format change?** All nine were captured
   over several sessions and the submodule has moved a long way since --
   including a rebase onto upstream `main`. A silently changed field order
   would produce exactly this.
3. **Does forcing a tile re-upload after load fix it?** If so the state is
   fine and only the post-load refresh is missing.

Note this may be long-standing rather than new: until the SDL3 fix, a black
window would have hidden it completely on that backend, and nobody had reason
to load an old state and stare at the tiles on the SDL2 one.
