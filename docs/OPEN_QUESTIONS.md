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

Two follow-ups it raises:

- **Which zone does each of `$0b8b`, `$0b8f`, `$0b93` count?** Two are
  weighted `*8` against the third. Cheap to settle: build only one zone type
  and watch which counter moves.
- **Is `$0b1d` the loan?** While nonzero it decrements and charges 500 per
  year. That reads like loan repayment, but it is inferred from code shape
  and should be confirmed by taking a loan in play — the user has already
  done so once.

### C5. Smaller open threads

- Moving-object entity identities (the sprite/vehicle table).
- What `03:9035`'s 6-slot window smooths.
- Bank-01 UI block layout generally.

---

## D. Game features

### D1. Sylt hack as a 9th scenario entry after Las Vegas

Nice-to-have, tracked as task #1. The map format is fully solved and verified
byte-exact (`docs/REFERENCE_map_format.md`), so this is mostly scenario-table
plumbing rather than new research.

---

## E. Verification gaps

- Coverage is the union of four recorded sessions. Rare paths — the end
  screen, disaster handling, some helper dialogs — may still be unrepresented,
  which would understate both the frontier and the executed-code percentages.
- The seven-save-state differential runs 410 frames with one scripted input
  pattern. A longer randomised-input differential would be stronger evidence
  that the COP widening is behaviour-preserving on paths not yet exercised.
