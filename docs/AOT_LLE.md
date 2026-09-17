# Static recompilation: AOT and LLE

How this project runs the game's code: the interpreter (LLE) that is the
correctness baseline, and the ahead-of-time (AOT) compiled banks that
`tools/regen.sh` generates from the ROM. This used to be the larger part of
README.md.

**In short, today:** the interpreter runs the game by default. When `src/gen`
is present the build also links the AOT tier; `SC_FIBER=1` runs the guest
inside a fiber that bounces into compiled bodies, which is how the decompiled
map generator runs.

## No full disassembly

This project does not aim at a complete, labelled disassembly of the game, and
will not produce one. Another fan project already works on that:
Vitor Vilela's SA-1 version,
<https://www.patreon.com/vitorvilela/posts/simcity-sa-1-112786310>.

No information from that project, or from its ROM, was used here --
deliberately. We know the project exists, but have never used it. Everything
in this repository was worked out from the original cartridge ROM by
execution, tracing and the analyzer described below.

Vitor Vilela, or anyone else, is welcome to bring the improvements made here
into their own work.

## Where it started: LLE first

This bring-up follows snesrecomp's own documented philosophy
(`snesrecomp/docs/LLE_FIRST_ANALYSIS.md`): the interpreter is the
correctness baseline for every game, and AOT-compiled banks are layered on
top only once proven. The first milestone ran the game entirely on the shared
interpreter tier (`interp816` over the real PPU/APU/DMA/cart device models),
driven by an accurate H/V master-clock frame loop -- the same technique
snesrecomp's own game-neutral reference driver
(`snesrecomp/cosim/ref_driver.c`) uses, so no game-specific scheduler or
address knowledge was required to reach this milestone.

`bash tools/regen.sh` already runs the real recompiler pipeline against the
ROM from nothing but the architectural vector-table seed in
`recomp/bank00.cfg` (no hand-identified function boundaries) and proves out
152 AOT-eligible / 48 LLE-only functions across 3 banks. Wiring those
generated banks in requires the AOT/`CpuState` hybrid runtime
(`common_cpu_infra.c` / `cpu_state.c` / `interp_bridge.c`) instead of the
standalone `interp816` this phase uses directly -- see the hybrid tier below.

## AOT frontier: where the static coverage actually stops

`tools/regen.sh` reports the analyzer's static frontier, and it is worth
tracking as the real progress metric for this project — the interpreter
runs everything today, so this number is what "static recompilation"
means concretely.

| | roots | exact variants | AOT-eligible | LLE-only |
|---|---|---|---|---|
| architectural vectors only | 26 | 314 | — | — |
| \+ screen-mode + map-path entries | 60 | 383 | 254 | 129 |
| \+ COP service entries | 71 | 411 | 275 (11,421 insns) | 136 (4,805 insns) |
| \+ power scan | 72 | 412 | 275 | 137 |
| \+ entries confirmed by execution (session 1) | 441 | 997 | 669 (24,244 insns) | 328 (9,797 insns) |
| \+ a second session (disasters, overlays) | 469 | 1038 | 695 (24,838 insns) | 343 (10,085 insns) |
| **\+ a third (every scenario, dialogs, endings)** | **487** | **1068** | **720** (25,448 insns) | 348 (10,150 insns) |

### The play-session loop is what actually moves this

The big step here came from playing the game rather than from reading it:


```bash
SC_PC_BITMAP_BANK=all SC_PC_BITMAP_PATH=coverage.bin ./build/Release/UrbanRecomp.exe us.sfc
```

Play — build, save, load, open every window — then close the window; the
bitmap is written on exit. Cross-referencing it against the manifest finds
addresses that an **executed** `JSR`/`JSL` actually called and that the
analyzer's closure does not already cover. One ~5-minute session over a
small city produced **369** such entries.

Execution coverage from that session was 22,284 ROM bytes (11.3% of the
code banks `00`-`05`), up from 6,284 measured headlessly — and 61% of what
executed was outside the analyzer's coverage, which is why the yield was
so large.

**It saturates fast.** Three sessions, each deliberately targeting
different ground:

| session | new addresses reached | new entry points |
|---|---|---|
| 1 — build, save, load, windows | 22,284 | **369** |
| 2 — disasters, overlays, other scenarios | 5,815 | 28 |
| 3 — every scenario, dialogs, endings | 1,641 | 18 |

The first session found almost everything. Later sessions still reached
genuinely new code, but the first session's roots had already let the
closure prove most of it statically, so the marginal yield collapsed.
Union coverage is 29,740 bytes, 15.1% of the code banks.

Against the union of both sessions, of everything that actually executed:

| | share of executed addresses |
|---|---|
| inside an AOT-eligible node | 57.1% |
| inside an LLE-only node | 41.4% |
| not analyzed at all | 13.4% |

So roughly **four in ten executed instructions still fall in code the
analyzer refuses to compile** — and that is the COP problem below, not a
shortage of roots. The AOT share of analyzed instructions has sat at
**~71% across every step of this table**, from 314 variants to 1068:
seeding roots grows the total and has never once moved the ratio.

The two things that moved it were both **indirect dispatch tables read
straight out of the ROM**, which is exactly what a static closure cannot
follow: the 23-entry screen-handler table behind `03:d289`'s
`JSR ($d255,X)`, and the 11-entry COP service table behind `00:8211`'s
`JSR ($8223,X)`. Neither was guessed; both were decoded and cross-checked
(COP service 8 is the LC_LZ5 decompressor, matching every
`LDA #$0008 ; COP #$00` call site in the ROM).

### The blocker: `COP` is this game's syscall instruction

This ROM uses `COP #$00` as a general syscall — **309 call sites**, with a
service number in `A` selecting one of 11 handlers. The analyzer treats
`COP` as a decode terminator (`insn.rs`: `if insn.mnem == "BRK" || insn.mnem
== "COP" { return false }`), so any function containing one is truncated
there and cannot be proven AOT-eligible.

Measured on the current manifest (1068 variants):

- LLE-only nodes named by a `cop_at_*` reason: **133**
- LLE-only nodes containing a `COP` in range: **145**
- union — COP-implicated: **159 nodes, 7,017 instructions**
- = **69% of all LLE-only instructions**, 20% of everything analyzed

`structural_poison` is implicated too, which is easy to miss. Poison is
*supposed* to be width refutation — proof a given `(pc, m, x)` never
occurs — but it is 4× enriched for COP-containing ranges against two
controls:

| node set | contains `COP #$00` in range |
|---|---|
| AOT-eligible (control) | 16% |
| other LLE-only (control) | 12% |
| **structurally poisoned** | **60%** |

and 210 of the 214 poisoned nodes sit at addresses that **actually
executed** in a recorded session, so they are real code, not data decoded
as code.

**Upper bound if this were fixed: AOT share 71.5% → ~91%.** An upper
bound, not a promise — some nodes would fail again for other reasons once
the decode continues past the `COP`.

### Fixed — and the estimate was too pessimistic

Implemented in this repo's submodule. The estimate above was made before
noticing the decisive hardware fact: **`COP` pushes P and the handler's
`RTI` pops it, so a COP is M/X-transparent.** Decode may therefore continue
past one in the same widths with no assumption at all, which is what made
this safe to do.

The framework already had the right primitive — `Break(tier_to_lle=True)`
emits `interp_tier_dispatch_tail`, executing the interrupt in the
authoritative interpreter and unwinding there rather than nesting a new one.
It was only reachable for COP-shaped bytes inside a declared `data_region`.
Outside one, a COP emitted a bare `/* COP: software interrupt */` comment —
the syscall silently skipped — which is exactly why those nodes had to be
poisoned. Tiering every COP makes the poison unnecessary.

| | before | after |
|---|---|---|
| exact variants | 1155 | **1584** |
| AOT-eligible | 845 | **1474** |
| LLE-only | 310 | 110 |
| instructions analyzed | 40,800 | **67,724** |
| AOT share | 80.3% | **94.8%** |

The frontier itself grew by 27,000 instructions, because decode now
continues past 552 COP sites instead of stopping at them. That is the part
the upper bound could not have predicted: it assumed a fixed denominator.

The more meaningful measure is against what the game actually runs. Taking
the union of four recorded play sessions — 31,487 distinct executed
instruction addresses — and asking where each one now lands:

| | share of executed code |
|---|---|
| inside an AOT-eligible node | **96.1%** |
| inside an LLE-only node | 3.6% |
| not in the frontier at all | 0.3% |

That last row was 0.7% until `03:8000` was declared as a root — the monthly
simulation tick, executed in every session but named by no `JSR`, `JSL` or
`JMP` anywhere in the ROM.

Verified: both tiers `--qualify 600` identical on every counter including
master cycles; all **seven** save states replayed with input give
**byte-identical 128 KB WRAM** between the interpreter and AOT tiers; 50
analyzer tests and 81 project tests pass.
[`docs/UPSTREAM_cop_syscall.md`](docs/UPSTREAM_cop_syscall.md) is a
filing-ready write-up.

### Measuring exit M/X instead of proving it

The remaining LLE-only code is almost entirely one problem: an unpublished
callee exit width truncates every caller, transitively, and the solver has no
fixed point for mutually recursive dispatch cycles. The machine has no such
difficulty — it just runs them — so `SC_MX_BITMAP` records the executed-PC
bitmap split four ways by the live `(m,x)`, which turns the proof obligation
into an observation.

Three tools use it, and the split matters: measuring is easy, and the whole
risk lives in the checking.

| tool | question |
|---|---|
| `tools/mx_exit_report.py` | what widths do unproven callees return in? |
| `tools/mx_exit_check.py` | do the analyzer's *published* exits match the machine? |
| `tools/mx_exit_propose.py` | which measurements are solid enough to emit as `exit_mx_at`? |
| `tools/gen_align_check.py` | does the emitted C decode on the boundaries the CPU actually used? |

**The analyzer is sound wherever it publishes**: 473 call sites checked, 473
agree, 0 mismatches, across both flags. 150 of those publish an exit width
that *differs* from the entry width, so the check is not agreeing trivially,
and fault-injecting flipped exit modes into the manifest does make it fail.

An earlier run of this comparison reported "40 mismatches", then 26 after two
confounds were corrected. All of them were artefacts of the check. The last
and least obvious: a call site carries a demand *per variant*, and comparing
the machine against the wrong variant's published exit accuses the solver of
disagreeing with code that never runs. `JSR` changes neither M nor X, so the
width recorded at the call site's own address names the entry variant — which
costs no new instrumentation and resolves all 26.

Three measured directives were emitted, derived independently from two
disjoint halves of an 18-run recording. One of them, `exit_mx_at 038df1 0 0`,
unblocked **`03:8000`, the monthly simulation tick** — the worked example
that `docs/OPEN_QUESTIONS.md` A1 had named as the thing this whole approach
existed to fix.

| | before | after |
|---|---|---|
| AOT-eligible | 1474 | **1475** |
| LLE-only | 110 | **109** |
| AOT share (analyzed) | 94.83% | **94.93%** |
| AOT share (executed code) | 96.1% | **96.5%** |

The modest headline number understates it: the remaining LLE-only set is now
67 nodes of genuine width refutation (715 instructions, the `brk_at_*` poison
working as designed) and 42 nodes blocked only by an unproven callee exit
(2,727 instructions). The ceiling for this line of work is **99%**.

What stands between here and that ceiling is *coverage*, not method. The five
bank-01 UI handlers are worth 766 instructions each and are the bulk of what
is left. An earlier reading of this said only interactive play could reach
them; that was wrong, and the reason is worth recording. The scripted input
had been written with the `$4218`/`$4219` word layout, where B/Y/Select/Start
land on bits 12-15 -- not button bits at all -- while the runner's
`input*_currentState` takes *serial* order, `B=$0001 .. R=$0800`
(mstan/snesrecomp#17). Those four buttons were never actually pressed, so the
UI state machine never advanced.

With correct masks, headless replay drives the UI. Combined with two recorded
play sessions and a fix to how the proposer bounds a routine's body -- it used
the node's `max_pc24`, which both swallows nested routines and stops short of
a truncated one's real return -- **all five handlers now publish measured
exits, every one `m0x0`, and the cycle is broken.**

| | before | after |
|---|---|---|
| AOT-eligible variants | 1,475 | **1,531** |
| LLE-only | 109 | **93** |
| AOT instructions | 64,386 | **67,178** |
| executed code inside an AOT node | 96.5% | **97.6%** |

The *analyzed* share reads lower afterwards (94.93% -> 90.08%) purely because
the denominator moved: publishing an exit lets decode continue past a call
that used to truncate, so the frontier grew from 67,828 to 74,576 instructions
and edges from 4,518 to 6,632 -- the same effect the COP work had, where the
frontier grew by 27,000. Absolute AOT instructions and executed-code share
both rose.

Verified after regeneration: 1,109 call sites checked and 1,109 agree with 0
mismatches, 790/790 on a holdout half alone, `gen_align_check` clean over
16,039 emitted labels, 81 framework tests pass, and all eleven save states
byte-identical in 128 KB WRAM between the two tiers.

`tools/gen_align_check.py` also turned up a **pre-existing decode
desynchronisation** in the emitted C, unrelated to the directives: a cfg
`func` with no `entry_mx_at` defaults to `M1X1`, and `00:926d` is only ever
entered with `x=0`, so that variant read a three-byte `LDY #$0000` as two
bytes and emitted a block label in the middle of an instruction. Dead code,
but `aot_eligible` with an empty `reasons` list — the same silent signature
as the inline-argument bug. Fixed with `entry_mx_at 926d 1 0`.

Declaring the service targets as roots (done above) makes the handlers
themselves reachable, but it cannot help the *callers*: the caller still
has an unprovable edge mid-function. Reaching high static coverage on this
game needs the framework to model `COP #$imm` as a call-with-return
through the COP vector, the way it already models JSL dispatch helpers
with inline tables. That is an upstream `snesrecomp` change, not something
this repo can fix in a cfg.

## The AOT/CpuState hybrid tier

To actually run the AOT-compiled banks `tools/regen.sh` already produces
(rather than 100% interpretation), the host needs to move from the
standalone `interp816` driver to the shared `CpuState`/`common_cpu_infra.c`
runtime and understand how the game's own main-loop idiom yields control
back to the host once per frame (`snesrecomp/docs/LLE_SCHEDULER.md`
describes the general "auto-quiescent" interpreter mechanism every new
game is meant to use for this, in preference to Mega Man X's older
per-game cooperative-scheduler/fiber approach).

### The game's vblank-wait idiom: found

That doc says the only per-game knowledge the LLE scheduler tier needs is
*"which PCs are the yield/die primitives"*. For this game that is
**`00:930d`, reached as `COP #$00` with `A = 0`** — the most-used service
in the ROM at 133 call sites:

```
00:930d  SEP #$20
00:930f  STZ $b9          ; clear the frame flag
00:9311  INC $c7          ; free-running spin counter
00:9313  LDA $b9
00:9315  BEQ $9311        ; spin until NMI releases it
00:9317  RTS
```

and the NMI handler closes the loop at `00:80bc` with `INC $b9` (gated on
bit 7 of `$00b1`, which `COP` service 4 at `00:8e75` sets). Confirmed by
bsnes trace, not inferred.

That also explains `$c7`: it counts spin iterations spent waiting for
vblank, and `00:823e` seeds the PRNG from it (`LDA $c7` → `$59`/`$5b`/`$5d`)
— timing-derived randomness, which is why the generated map depends on how
long the player took to get there.

This is the seam the hybrid tier needs. It is a plain `RTS`-returning
primitive rather than Mega Man X's coroutine switch, so it should suit the
fiber-free `hle_func` + NLR-unwind pattern that doc describes without the
stack-corruption problem MMX's yield had.

### Migration progress

| step | state |
|---|---|
| 1. Does the generated C build at all? | **done** — `UrbanRecompAOTProbe`, 312,768 lines compile and link, 720 compiled variants across 534 dispatch rows |
| 2. Can both tiers live in one binary? | **done** — the same `src/main.c` linked with the generated banks and the AOT runtime. While the AOT build was still pure interpreter this produced byte-identical `--qualify` output to the shipping build; now that the fiber drives compiled bodies by default the counters differ, because the work itself differs (see 3e) |
| 3a. Is any compiled body *correct*? | **8 bodies verified** — `UrbanRecompAOTDiff` runs each against the real ROM routine over 8 randomised trials: 64/64 identical WRAM + A/X/Y, zero divergences, every body returns `NORMAL` |
| 3b. Declare the frame boundary | **done** — `hle_func 930d ScHle_WaitForVblank` in `recomp/bank00.cfg`, implemented in `src/sc_hle.c`. The emitter now routes all four M/X variants of `bank_00_930d` through the host function |
| 3c. Fiber layer for the frame boundary | **done** — `src/sc_fiber.c`, verified by `tests/fiber_test.c` (stack and FP state preserved across switches) |
| — | **blocked on a correctness bug**: 28 compiled call sites execute instructions the ROM never runs ([`docs/UPSTREAM_inline_args.md`](docs/UPSTREAM_inline_args.md)) |
| 3d. Drive the guest inside the fiber | **done** — `src/sc_fiberdrive.c`, entered at `I_RESET_M1X1`. It is an interpreter-with-bouncing driver, since the game has no compiled entry point to start from ([why](docs/MIGRATION_step3.md) §5) |
| 3e. Replace real work with a compiled body | **done** — `hle_func f1ed ScHle_MapGen` runs the decompiled map generator (`src/sc_mapgen.c`) instead of the ROM's. Verified bit-exact on three maps across both generator branches: same 12000 cells, same draw count, same final PRNG state. The routine it replaces takes the SNES CPU ~800 frames; the map is now complete the frame after the trigger |
| 4. Make the AOT tier the default | **done, then taken back** — when `src/gen` is present, `UrbanRecomp` links the AOT tier, but the fiber is opt-in again (`SC_FIBER=1`). Playing on it brought back widescreen defects that had been fixed on the interpreter path, because those fixes hang off the per-opcode loop. The interpreter is the default and the correctness baseline |

The normal build links the AOT tier itself, so there is nothing extra to
build for it:

```bash
cmake --build build --target UrbanRecomp        # interpreter + linked AOT tier
SC_FIBER=1 ./build/Release/UrbanRecomp ...      # run the guest in the fiber
```

Two caveats worth stating plainly. The generated code is compiled against the
**US** ROM, so on any other region the fiber steps down to the interpreter and
says so — every region stays playable, and an explicit `SC_FIBER=1` there is
still refused rather than silently ignored. And without `src/gen` (a fresh
clone, before `tools/regen.sh`) the whole AOT block is skipped and the target
builds interpreter-only.

The diagnostic targets remain `EXCLUDE_FROM_ALL`:

```bash
cmake --build build --target UrbanRecompAOTProbe          # link probe
cmake --build build --target UrbanRecompAOT     # same content as the
                                                      # default build now;
                                                      # kept for the docs and
                                                      # scripts that name it
```

Step 2's point is narrow but load-bearing: the two tiers **share one WRAM
array**. `common_rtl.c` defines `g_ram[0x20000]` with the same `$7E`/`$7F`
semantics this host already uses, and this host passes `g_ram` straight to
`snes_init()`, so nothing has to be copied between tiers. `SC_AOT_TIER`
in `src/main.c` marks the handful of symbols that move ownership to the
runtime in that build (`g_ram`, `g_interp_apu_driving`, `ppudma_record_dma`,
`interp816_opcode_hook`) and the one the runtime expects the game to supply
(`g_spc_player`).

Step 3a started from the easiest possible subject — `00:824f`, pure WRAM
state, no I/O, no branches, no calls:

```
REP #$20 ; CLC ; LDA $59 ; STA $5d ; ADC $5b ; STA $59 ; ADC $5d ; STA $5b ; RTS
```

Seeded identically on both sides and compared: 8/8 match, including
`ffff/ffff/ffff`, `8000` overflow and `7fff+1`, which are where a carry bug
would show.

`tools/select_pure_leaves.py` then generalised it. Synthetic entry state is
only a *fair* test where the routine has no preconditions a real caller would
have established, so it selects bodies with no calls or non-local transfers,
no hardware-register access, and no stack manipulation. 34 of 722 AOT bodies
qualify; 8 of those return cleanly from synthetic state and are compared over
8 randomised trials each. **64/64 comparisons identical, zero divergences.**

Getting there meant fixing four harness artefacts, each of which first
presented as a codegen bug:

| symptom | actual cause |
|---|---|
| harness hangs forever | `00:930d` is the **vblank spin**, waiting on an NMI that never arrives. A compiled body is a plain C call with no way to interrupt it — so the interpreter now runs **first**, under a step guard, and a body that does not terminate is never called |
| segfault | `00:98a0` does `PLX ; PLA ; PHA` to pop its **own return address** and read inline arguments the caller emitted after the `JSR`. Handed a fabricated return address it indexes arbitrary memory |
| segfault | `01:b375` does `LDX $01f9 ; LDA $0180c0,X` — a table index taken **out of WRAM**. Fully random WRAM hands it a wild index; real callers keep it small, so the harness does too |
| one divergence at `00:8436` | its manifest extent ends at `$8448`, one byte before its own `RTS`, because the not-equal path continues into a separate node. The interpreter stopped early while the body carried on. Comparison now requires the interpreter to have genuinely **returned** |

That last one is the useful lesson for anyone extending this: "PC left the
recorded extent" and "the routine returned" are different events, and only the
second makes a comparison meaningful. The 24 bodies skipped for no clean RTS
are not failures — they are routines whose contract synthetic state cannot
satisfy.

Step 3b is the real work: driving the frame loop through
`interp_bridge_run_scheduler` against `00:930d`/`$b9`, then the differential
gate `LLE_SCHEDULER.md` specifies (bounced vs interpreted must be bit-exact).
The COP limit starts to bite there in practice — roughly a third of executed
code has no compiled body to bounce into.

**ar-recomp comparison** (was a TODO, now done -- see
[`docs/MIGRATION_step3.md`](docs/MIGRATION_step3.md) §4):
[ar-recomp](https://github.com/DerrickGold/ar-recomp) links its generated
banks and runs the game inside a **fiber**, one switch per frame, with the
ROM's vblank wait replaced by an HLE that yields back to the host
(`hle_func 8418 ActRaiser_WaitForVblank`). There is no per-opcode
interleaving; the host owns the frame boundary and renders through
`draw_ppu_frame`. Its HLE surface is only 13 functions in total, and it
declares 1,512 `func` boundaries in bank 00 alone against this project's
~500 across all banks.

The mapping to this game is direct: `00:930d` (COP service 0, the vblank spin
on `$b9`) is our `WaitForVblank`, and it is a plain `RTS`-returning routine
rather than a coroutine switch. The one open question is fibers vs the
framework's newer fiber-free LLE bridge, which `LLE_SCHEDULER.md` says is
replacing them -- but the first step, declaring `hle_func 930d`, is required
by both.
