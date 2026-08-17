# Step 3b: what routing execution through compiled bodies actually requires

Steps 1–3a are done: the generated banks build, link, share a binary and a WRAM
array with the interpreter, and 8 compiled bodies have been differentially
verified against the real ROM routines (64/64 comparisons identical). This
records what stands between that and actually *running* compiled code, because
it is more than wiring and should be understood before anyone starts.

## 1. The two tiers have incompatible timing models

This host's frame loop is **per-opcode and dot-accurate**:

```c
int cyc = interp816_runOpcode(cpu);
int master = cyc * 8;
for (int i = 0; i < master; i += 2) handle_pos_stuff();   /* PPU H/V position */
snes->apuCatchupCycles += (double)master * kApuCyclesPerMaster;
snes_catchupApu(snes);
```

Every opcode advances the PPU dot by dot. That model is why this project could
find and fix the missing-HDMA bug, and it is what the `--qualify` baseline
rests on.

The AOT runtime's model is **per-frame**: `RtlRunFrame` (`common_rtl.c:421`)
calls `g_rtl_game_info->run_frame()`, a per-game function that runs a whole
frame of guest code, and the PPU is advanced around it rather than inside it.
`interp_bridge` tracks `cpu->master_cycles` and does APU catch-up itself
(including a batching optimisation that collapsed interp-heavy frames ~250x)
but does **not** step the PPU per opcode.

So a compiled body runs many opcodes with no dot-level device advance. Three
ways out, in increasing order of disruption:

1. **Advance devices by the body's cycle delta on return.** `CpuState` carries
   `master_cycles`, so after a compiled call the host can run
   `handle_pos_stuff()` for the elapsed master cycles. Total timing stays
   right; intra-routine dot accuracy is lost. Fine for pure computation,
   wrong for anything that touches PPU registers mid-scanline.
2. **Only bounce into bodies proven not to touch hardware** — the same
   pure-leaf property `tools/select_pure_leaves.py` already computes. Safe,
   but limits the win to exactly the routines that matter least.
3. **Adopt the runner's frame model** wholesale. Most faithful to the
   framework, and the largest change: it replaces the loop this project's
   accuracy story is built on.

   An earlier draft of this section claimed option 3 would put the HDMA and
   audio work at risk. **That was overstated** — see the ar-recomp section
   below. Dropping per-opcode interleaving does not mean dropping per-scanline
   rendering: the host still advances the PPU line by line around the game's
   execution, so raster effects keep working. ar-recomp ships widescreen and a
   3D diorama on exactly this model.

Section 4 below revises the recommendation accordingly.

## 2. What it would buy, measured

Of twelve routines identified as hot or structurally important:

| routine | tier |
|---|---|
| `00:824f` PRNG step | **AOT** |
| `00:90dd` LC_LZ5 decompressor | **AOT** |
| `00:930d` vblank wait | **AOT** |
| `00:929b` edge detector | **AOT** |
| `03:d15f` map unpacker | **AOT** |
| `01:f1f1` terrain generator | **AOT** |
| `03:a390` 32-bit multiply | LLE |
| `00:8211` COP dispatcher | LLE |
| `03:b152` power scan | LLE |
| `03:ce2e` scenario map loader | LLE |
| `03:ddb6` scenario select | LLE |
| `03:e2ee` win-mark setter | LLE |

Six and six. The misses are not random — each has a specific cause, and two of
them matter a lot.

### The COP dispatcher is itself LLE, which compounds

`00:8211` fails with `truncated_call_continuation`, because its whole body is
`JSR ($8223,X)` — an indirect call through the service table. So it is not just
that COP truncates every *caller* (the finding in
`docs/UPSTREAM_cop_syscall.md`): the dispatcher those callers reach is
uncompilable too, so **every one of the ~309 COP call sites lands on the
interpreter no matter what**. `03:ce2e`, the scenario map loader, shows the
caller half of the same problem directly: `cop_at_03CE5C`, `structural_poison`.

### One unproven callee exit blocks 7,643 bytes

`03:a390`, the 32-bit software multiply and the simulation's arithmetic
workhorse, sits inside a single node spanning `03:90c5`–`03:aea0` — 7,643 bytes
— that is LLE for:

```
unproven_call_at_03ABF2_to_03AC5B_m0x0
truncated_call_continuation
unproven_callee_exit
```

The blocking site is tiny:

```
03:abf0  BNE +3
03:abf2  JSR $ac5b      <- this call
03:abf5  RTS
```

and the callee begins `REP #$20 ; PHD ; TDC ; SEC ; SBC #$0008 ; TCD` — it
allocates an 8-byte direct-page frame. The analyzer cannot prove which (m, x)
it returns with, and that one unproven exit costs the whole 7.6KB node.

**Measured** (`SC_ADDR_TRACE=03:abf5`, which reports live M/X at the
instruction after the call returns): every observed return is **m16 x16**.

That is *not* offered as a fix. It is 7 samples from a single save state —
states 1 and 3 never reach the call at all — and a routine can exit at one
width on the path you sampled and another on a path you did not. Declaring
`callee_exit_mx` on that evidence would change codegen on the strength of an
under-sampled guess, which is worse than leaving it interpreted. It is recorded
because it is a concrete, reproducible data point for the upstream exit-mode
inference: `LLE_FIRST_ANALYSIS.md` says exit-mode **sets** are implemented, so
a case where inference still fails is worth someone's attention.

### `03:b152` is a genuine width refutation, not a defect

The power scan's `m1x1` variant fails with `brk_at_03B168` + `structural_poison`,
and that is the analyzer working correctly. The routine's prologue contains
`SBC #$0008` — an m-dependent immediate. Decoded with 8-bit A, `e9 08 00`
becomes `SBC #$08` followed by `00` = `BRK`. The poison is proof that the
`m1x1` entry never really happens, exactly as the poison-driven width
refutation in `LLE_FIRST_ANALYSIS.md` describes.

## 3. Order of work, if resumed

1. Decide the timing model (§1). §4 answers this by example — ar-recomp takes
   option 3 and ships it. What remains is the fibers-vs-LLE-bridge fork, and
   step 2 below is required either way.
2. Declare `hle_func 930d` and give it a host implementation — the analogue of
   ar-recomp's `hle_func 8418 ActRaiser_WaitForVblank`. This is what makes the
   game's frame boundary visible to the host, and both designs need it.
3. Then either yield a fiber from that HLE (ar-recomp's model), or drive the
   frame through `interp_bridge_run_scheduler(cpu, entry, 0x009313,
   0x00b9)` — SimCity's yield primitive is `00:930d` spinning on `$b9`, and it
   is a plain `RTS`-returning primitive, so it should not need MMX's
   coroutine-switch handling. Note the **auto-quiescent** variant is wrong for
   this game: the spin does `INC $c7` every iteration, so the state is not
   read-only and the quiescence detector will never fire.
4. Run the differential gate from `LLE_SCHEDULER.md`: bounced vs interpreted
   must be bit-exact over the attract demo. This project already has the tools
   for it — `--qualify` hashes logic/video/audio per frame, and
   `SC_PC_BITMAP_BANK=all` shows which paths each side took.
5. Only then consider whether the COP and exit-mode limits are worth chasing
   upstream first; with a third of executed code having no body to bounce into,
   the measured win may be small enough to change the plan.


## 4. How ar-recomp does it — and why that changes the recommendation

`ar-recomp` (Derrick Gold's ActRaiser recompilation, sitting alongside this
repo) is a mature project on the same framework, and it answers the §1 question
by demonstration: it takes **option 3**, and it works.

### The mechanism

1. **`src/gen` is linked.** Compiled code is the main execution path, not an
   experiment. For scale: ar-recomp's `recomp/bank00.cfg` alone declares
   **1,512** `func` boundaries (bank01 128, bank03 107) against this project's
   ~500 across all banks.
2. **One fiber per game frame.** `RunOneFrameOfGame()` (`src/actraiser_rtl.c`)
   does `SwitchToFiber(g_game_fiber)` on Windows, `swapcontext` elsewhere. The
   compiled game code then runs continuously inside that fiber.
3. **The vblank wait is HLE'd.** `recomp/bank00.cfg` declares
   `hle_func 8418 ActRaiser_WaitForVblank`: the ROM's wait routine is replaced
   by a host C function that calls `ActRaiser_YieldToHost()`, suspending the
   fiber back to the host.
4. **The host owns the frame boundary.** It sets `g_snes->forceNmi` and
   `nmiAvail` (a fresh RDNMI vblank token), switches in, and on the yield
   renders through `draw_ppu_frame`.

There is no per-opcode interleaving anywhere in that loop.

The HLE surface is **small**: 13 `hle_func` declarations in total, and most are
optimisations (sprite building, camera) rather than necessities. The vblank
wait is the one that makes the model work.

Two details worth stealing if we go this way: the fiber is created with
`FIBER_FLAG_FLOAT_SWITCH` (mandatory — without it x86 FP state is not switched
and the coroutine does use FP), and the watchdog gets a *yield* hook rather
than a `longjmp`, because longjmp out of a fiber is undefined behaviour.

### The mapping to SimCity is unusually direct

| ar-recomp | SimCity |
|---|---|
| `hle_func 8418 ActRaiser_WaitForVblank` | `00:930d` — COP service 0, spins on `$b9` |
| HLE yields the fiber | plain `RTS`-returning; simpler than a coroutine switch |
| host re-arms NMI to release the wait | `00:80bc` `INC $b9` — already identified |

Every piece that declaration needs is already established in `ROM_MAP.md`.

### Revised recommendation

Follow ar-recomp. It is a working, inspectable implementation of the exact
problem, on the same runtime, for a more complex game — which beats reasoning
from first principles about a model nobody here has run.

The caveat is that it uses **fibers**, and `snesrecomp/docs/LLE_SCHEDULER.md`
calls that "Mega Man X's older per-game cooperative-scheduler/fiber approach"
and states the framework is retiring it in favour of the fiber-free LLE bridge
(`interp_bridge_run_scheduler` + bouncing). So this is a real fork:

- **ar-recomp's path** — proven, copyable, and the pattern is on disk.
- **The framework's stated direction** — newer, aligned with upstream, and
  SimCity's `RTS`-returning primitive suits it better than MMX's coroutine
  switch did (which is what forced the NLR-unwind machinery in the first
  place).

Either way the first concrete step is identical: declare
`hle_func 930d` and give it a host implementation. That is what makes the
game's frame boundary visible to the host, and it is required by both designs.


## 5. Decision: fibers — and the one way SimCity differs from ar-recomp

Fibers chosen. `src/simcity_fiber.c` implements the coroutine layer
(`SimCityFiber_Create` / `_RunOneFrame` / `_YieldToHost`), with
`tests/fiber_test.c` as a standalone self-test: it drives five frames, yields
from eight stack frames down, and checks that every level's local survives the
switch and that floating-point state is preserved on both sides. Coroutine
bugs are far cheaper to find there than underneath a running 65816.

`FIBER_FLAG_FLOAT_SWITCH` is mandatory, as ar-recomp documents — without it x86
FP state is not switched between fibers, and both sides here use FP.

### SimCity cannot start inside compiled code

This is the one place the ar-recomp transplant breaks, and it shapes
everything after it.

ar-recomp's game coroutine begins with `ResetHandler_M1X1(&g_cpu)` and never
returns: the whole game runs as compiled C inside that single call, and the
fiber exists only to suspend it. SimCity has no compiled entry point to hand
the fiber, because **both architectural entry points are `lle_only`**:

| entry | blocked by |
|---|---|
| `I_RESET` `00:8000` | `unproven_call_at_008056_to_03D283` |
| NMI `00:80b2` | `unproven_call_at_00813A_to_00C3F9` |

`03:d283` is the screen-mode dispatcher (`LDA $14 ; ASL ; TAX ;
JSR ($d255,X)`), so its exit modes are unprovable through the indirect call,
and that one unproven call leaves the reset vector uncompiled.

So the fiber must wrap an **interpreter that bounces into compiled bodies**,
not host compiled code directly — a hybrid of the two designs. That is the
honest shape for a game where 41% of executed addresses have no compiled body
at all, and it means the eventual driver looks more like
`interp_bridge`-with-a-fiber than like ar-recomp's pure coroutine.

### Not yet wired, deliberately

`g_simcity_yield_to_host` is still NULL. Installing
`SimCityFiber_YieldToHost` before a host frame driver exists would yield into
a host that is not inside `SimCityFiber_RunOneFrame`, which crashes. The
remaining work is that driver: run the guest inside the fiber, bounce
JSR/JSL into compiled bodies where one exists, and let the vblank HLE suspend.


## 6. The tiers agree on logic but not on cycle counts

Measured while checking whether a device-advance-by-cycle-delta driver is
viable. `SimCityAOTDiff` now compares `cpu->master_cycles` against the
interpreter's summed opcode cycles over the same routine and inputs:

```
identical WRAM + A/X/Y : 8 of 8 bodies   (64 of 64 trials)
cycle counts agree     : 51 of 64 trials
```

So the emitted code computes the right answer every time and charges the wrong
number of cycles about a fifth of the time. Every mismatch is the **AOT
counting more**, never less:

| routine | AOT | interp | delta |
|---|---|---|---|
| `00:8924` | 760 | 744 | +16 master (+2 CPU) |
| `00:8982` | 760 | 744 | +16 master (+2 CPU) |
| `00:d23a` | 240 | 232 | +8 master (+1 CPU) |

The opcodes point at data-dependent penalties. `00:d23a` is
`LDA $0421 ; ASL ; TAX ; LDA $d193,X ; STA $7e2000 ; RTS` — an **absolute
indexed** read, which costs an extra cycle only when the index crosses a page
boundary, and its error is exactly +1 CPU cycle. `00:8982` and `00:8924` are
branch ladders (`LDA abs ; BNE ; STZ abs ; LDA abs ; BNE ; RTS`), where a taken
branch costs +1 and a taken branch that crosses a page costs +2 — their error
is exactly +2.

**Hypothesis** (opcode evidence, not proof): the emitted code charges these
penalties unconditionally while the interpreter charges them only when they
actually occur. That fits every observation, including the direction — an
unconditional worst-case charge can only ever be too high — and the fact that
only *some* trials mismatch, since the randomised inputs decide whether a page
is crossed.

### What it means for the driver

Advancing the PPU by the compiled body's `master_cycles` delta — option 1 in §1
— is therefore not timing-neutral. It over-advances slightly, per bounce, in a
data-dependent way. With thousands of bounces a frame the drift is not obviously
negligible, and it means a **bounced-vs-interpreted differential will diverge on
`master=` even when the logic is bit-identical**. Anyone running that gate should
expect it and compare logic/video hashes rather than the master clock, or the
first run will look like a codegen failure.

This is worth an upstream question in its own right: two tiers of the same
framework disagreeing on cycle counts for the same routine is a portability
problem beyond this game.

## 7. Step 3d attempted: the guest does run inside the fiber, and then deadlocks

`SC_FIBER=1` on the AOT build now creates the game fiber, installs the vblank
yield, and runs the guest inside it. `src/simcity_fiberdrive.c` is the driver;
it is strictly opt-in and the default path is untouched, verified byte-identical
between tiers on five save states with the fiber code linked in but inert.

### §5's premise is half obsolete

§5 said the ar-recomp transplant was impossible because "there is no compiled
entry point to hand the fiber" -- both architectural entries were `lle_only`.
**RESET is compiled now.** `03:d283`, the screen-mode dispatcher whose
unproven exit blocked it, is `aot_eligible`, so `008000:M1X1` is too, with an
empty `reasons` list, and the dispatch table carries it as `I_RESET_M1X1`.
NMI is still LLE, but does not need compiling for this model -- the host owns
the frame boundary and releases the wait itself, as ar-recomp's does.

So the straight transplant *is* available for the reset path, and it was taken.
Measured: the fiber switch works and the compiled reset handler is entered.

```
[fiber] driving the guest inside the fiber (entry I_RESET_M1X1)
[fiber] entering I_RESET_M1X1
<hangs>
```

### The real blocker is device-register spins, not the entry point

The guest never reaches the vblank HLE, so the fiber never yields and the host
never regains control. The cause is structural, and §1 has it in outline
without drawing the conclusion:

**Devices only advance while the host holds the fiber.** So any guest loop that
spins on a device register waiting for hardware to change deadlocks -- the
guest cannot make progress, and the host cannot advance the device that would
release it.

The boot path contains exactly such a loop, and it executes in every recorded
session:

```
00:9280  LDA $4212
00:9283  AND #$01
00:9285  BNE $9280      ; spin until auto-joypad read is not busy
```

`$4212` bit 0 clears as the beam advances. Under the per-opcode interpreter
that happens naturally; inside the fiber it never does.

### What that means for the design

ar-recomp gets away with a pure coroutine because its waits are vblank-shaped
and HLE'd. SimCity has at least one hardware-status spin *before* the first
vblank wait, so the pure transplant cannot boot no matter how good the
coverage gets. Three ways forward, and the first is the cheapest:

1. **HLE the status spins too.** `hle_func 927c` and any siblings, the same
   way `hle_func 930d` handles the vblank wait. ar-recomp declares 13 HLEs
   and calls most of them optimisations; SimCity would need a few as
   *necessities*. Requires finding them all -- a missed one is another
   deadlock, and it will look exactly like this one.
2. **Let the fiber yield on device reads.** Any read of a status register
   while the guest holds the fiber becomes a yield point. General, no
   per-routine knowledge needed, but it puts a yield check on the hot path.
3. **`interp_bridge_run_scheduler` instead of a bare call.** The framework's
   own model, and the direction `snesrecomp/docs/LLE_SCHEDULER.md` says it is
   moving in. Note `interp_bridge_lle_master_deadline_reached` -- which every
   generated block already polls -- is inert outside the scheduler: it
   requires `s_lle_sched_depth > 0 && s_interp_bounce_owner_depth > 0`. A bare
   `I_RESET_M1X1()` call therefore has **no bound on execution at all**, which
   is why the hang is unbreakable from the host side rather than merely slow.

That last point is the strongest argument for option 3: the runtime already
has the bounding mechanism, and calling a compiled body directly opts out of
it. §5's guess that the eventual driver "looks more like `interp_bridge`-with-
a-fiber than like ar-recomp's pure coroutine" survives, for a different reason
than it gave.

## 8. Option 3 tried too: the two device models are the real blocker

§7 ended by recommending `interp_bridge_run_loop` -- the framework's own
model, which restores the execution bound and keeps compiled bodies live. It
was implemented (`src/simcity_fiberdrive.c`) with SimCity's wait mapped
exactly:

```
interp_bridge_run_loop(&cpu, resume_pc24,
                       0x009311,   /* 930d's spin: INC $c7 ; LDA $b9 ; BEQ */
                       0x00b9,     /* the flag the NMI handler sets */
                       0x00);      /* cleared while waiting */
```

**It hangs in the same place, and the reason is the same one wearing a
different coat.** `interp_bridge_run_loop` never returns from its first call,
and `SNESRECOMP_YIELD_DIAG=1` prints nothing -- the guest never reaches
`00:9311` at all.

The bridge advances the APU (`snes_catchupApu`) and accumulates
`cpu->master_cycles`. **It never advances the PPU beam.** Upstream that is
correct, because the host advances the PPU around `RtlRunFrame`, per frame.
This host advances it *per opcode*, from `handle_pos_stuff()` in
`src/main.c`, and nothing outside that loop drives it. So `$4212` is frozen
inside the bridge exactly as it was inside the fiber, and `00:9280` spins
there too.

There is no hook to bridge them with. `interp816_opcode_hook` is declared in
`interp816.h` and defined as a no-op in `interp_bridge.c`, and is **never
called anywhere in the runner** -- a dead extension point.

### The finding, stated plainly

Every driver design fails at the same place, and it is not the entry point,
not the fiber, and not AOT coverage:

> **This host owns a per-opcode device model that no framework driver drives.**
> Any design that runs the guest outside `src/main.c`'s opcode loop freezes
> the PPU, and the ROM's boot spins on a beam-derived register before it ever
> reaches a vblank wait.

That reframes §1. Its three options were about *timing fidelity* -- how much
dot accuracy a driver gives up. The real question is more basic: **who steps
the beam.** Until something outside `handle_pos_stuff()` can, no driver boots
at all, faithful or otherwise.

Two ways to close it, both structural:

1. **Give the bridge a per-opcode host callback.** Upstream change; the dead
   `interp816_opcode_hook` is the natural place. Small, and it would let this
   host keep its per-opcode beam -- the thing the accuracy story and the HDMA
   fix rest on.
2. **Adopt the runner's per-frame model** (§1 option 3). Larger, and it
   replaces the loop this project's `--qualify` baseline is built on. ar-recomp
   ships widescreen on it, so it is not disqualifying -- but it is a rewrite,
   not a wiring job.

Option 1 is the smaller change and preserves more. It is also the one that has
to go upstream, which makes it the third candidate PR alongside `exit_mx_set`
and the COP fix.

### State on disk

`SC_FIBER=1` is wired end to end and does not work; everything else is
unaffected. The default path is byte-identical between tiers on all eleven
save states with the driver linked in, the fiber self-test passes, and 81
framework tests pass. The driver is kept rather than reverted because the
diagnosis is in it, and because both remaining options reuse most of it.

## 9. Driving through the bridge: two blockers removed, one left

`SC_FIBER=1` now drives the guest through `interp_bridge_run_loop` rather than
calling a compiled body directly. The mapping is right and two real obstacles
are gone; it still does not boot, and the remaining cause is now pinned rather
than guessed.

### Removed: the auto-joypad spin

`00:9280 LDA $4212 ; AND #$01 ; BNE $9280` waits on auto-joypad-busy, from
`00:8151` and `00:8201` -- both per-frame, in the NMI path, not boot-only. But
`$4212` bit 0 is literally `autoJoyTimer > 0` in `snes.c`, armed with 4224 at
vblank start and counted down by the beam.

So the host can simply **not hand over a frame whose input latch is still
busy**: `sc_advance_until_input_ready()` in `src/main.c` drains it before the
guest runs. ~4224 master cycles at the top of the frame, no HLE, no
per-routine knowledge. This is the general shape of the answer for any
hardware-status wait -- advance the device before handing over, rather than
HLE-ing the routine that waits on it.

### Removed: the missing execution bound

`interp_bridge_set_master_deadline(cpu->master_cycles + 357368)` is now armed
per frame. The bridge's own step cap counts **interpreted steps only**;
once it bounces into a compiled body nothing counts, and every generated block
polls `interp_bridge_lle_master_deadline_reached()` which returns false unless
a deadline is set.

### Left: something inside the bridge never returns

Measured, with `SC_FRAME_TRACE=1`:

```
[frame 1] a: draining input latch
[frame 1] b: latch drained, bridge at 008000
   <no "c: bridge returned">
```

So the latch drain completes, `interp_bridge_run_loop` is entered at the reset
vector, and it never comes back. Three things are ruled out by measurement:

- **not an interpreter loop** -- `SNESRECOMP_INTERP_STEP_CAP=200000` still
  hangs; a spinning interpreter would bail and return 0.
- **not the vblank wait** -- `SNESRECOMP_YIELD_DIAG=1` prints nothing, so
  `00:9311` is never reached.
- **not the auto-joypad latch** -- drained before entry, and the beam is
  frozen inside the bridge so it cannot re-arm.

That leaves an unbounded loop inside a **compiled** body, with the deadline
not firing. The likely reason, unconfirmed: the guard is

```c
s_lle_sched_depth > 0 && s_interp_bounce_owner_depth > 0
```

and `run_loop` sets the first but only the bridge's own bounce path sets the
second. A body entered through the generated dispatch table's tier call rather
than through that path would poll a deadline that is permanently inert. If
that is right, the bound cannot be armed from the host at all and it is an
upstream fix.

### Correction: it is not a hang, it is ~1000x too slow

Two readings in this section were wrong, both from the same mistake --
treating a killed timeout as a deadlock.

Adding markers either side of `interp816_runOpcode` shows the interpreter
advancing perfectly normally:

```
[pctrace] step=0 pre-runOpcode  pc=008000
[pctrace] step=0 post-runOpcode cyc=2 pc=00:8001
[pctrace] step=1 post-runOpcode cyc=2 pc=00:8002
[pctrace] step=3 post-runOpcode cyc=3 pc=00:8005
```

So **"stuck inside a single step on `CLC`" was wrong.** The earlier trace
printed only `step=0` because the modulo interval was 200,000 and the run
never got that far -- not because it never left step 0.

Measured rate: **~131,000 steps in 20 s, about 6,550 steps/sec.** A 65816
interpreter should manage millions. The default step cap of 2,000,000 needs
~5 minutes at that rate, which is why every 40-60 s timeout looked like a
hang. A 600 s run still did not return, so the true rate is lower again or
time is going somewhere the step counter does not see.

`SNESRECOMP_INTERP_NOAPU=1` (added for this bisect) does not change it, so the
bridge's APU catch-up is not the cost.

**This is a performance problem, not a deadlock**, which makes every earlier
"deadlock" conclusion in this section suspect -- including the fiber one in
§8. The fiber may equally have been slow rather than stuck; that was never
measured, only assumed from a timeout.

What is genuinely established: the mapping is right (`run_loop` at 00:9311 on
`$b9`), the latch drain works, the guest executes real instructions under the
bridge, and the host's beam advance is correctly ordered around it. What is
not established is why per-step cost is three orders of magnitude off.

Next step is profiling, not deadlock-hunting: attach a sampling profiler to a
`SC_FIBER=1` run, or bisect the per-step work by stubbing pieces of
`_interp_run_core`'s loop body the way `SNESRECOMP_INTERP_NOAPU` stubs the APU.
The auto-quiescent bookkeeping, the pre-opcode hook scan and the write-log
sync are all per-step and all candidates.

### Superseded: the hypothesis below was WRONG

Both diagnostics were added to the submodule
(`SNESRECOMP_DEADLINE_DIAG`, `SNESRECOMP_INTERP_PCTRACE`; commit `93d4dd1`)
and they refute it:

- `SNESRECOMP_DEADLINE_DIAG=1` prints **nothing**. Nothing is polling the
  deadline past its expiry, so the hang is not an unbounded compiled body with
  a suppressed bound. The depth-guard theory is dead.
- `SNESRECOMP_INTERP_PCTRACE=200000` prints **exactly one line**:

```
[pctrace] step=0 pc=008000 op=18
```

The interpreter never reaches step 200000. It is not looping over
instructions -- **it is stuck inside a single step, the first one**. `$18` is
`CLC`, which cannot hang.

So the problem is in the per-step host plumbing between the bridge and this
host, not in guest code, not in coverage, and not in the deadline. Everything
the bridge does around an opcode is suspect: the APU catch-up path
(`bridge_apu_flush`, `snes_catchupApu`, `apu_runToGuestCycle`), the
`g_interp_apu_driving = 1` the bridge sets on entry, or a bus access that
waits on a device this host advances only from `handle_pos_stuff()`.

Note this host defines its own `rtl_sync_apu_to_cpu_locked` / `RtlApuWrite` as
no-ops and they win the link over `common_rtl.c`'s (the LNK4006 warnings are
exactly that), so the bridge may be calling into APU plumbing that this host
has deliberately stubbed out -- which would fit "hangs on the first step"
better than anything in the ROM does.

### Next diagnostic

Attach a debugger, or bisect the loop body: the answer is inside one iteration
of `_interp_run_core`'s `for (; steps < step_cap; steps++)`, between the
`pctrace` print and the next one. Comment out the APU catch-up first -- that is
the only part of the per-step work this host has stubbed and the runner has
not.

The default path is untouched throughout: `--qualify 600` PASS and four save
states byte-identical between tiers with the frame-model code linked but
inert.
