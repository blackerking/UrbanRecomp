# Widescreen city map: current state, open items, and dead ends

Rewritten 2026-08-27 after the work settled. The previous version had grown by
accretion -- duplicate section numbers, four "superseded" blocks, and finished
items interleaved with open ones -- and several of its sections described
approaches that were later reversed. Anything still true was carried over.

**Return point: tag `rc1`.** If a change makes the picture worse, come back to
it rather than repairing forward.

## The fault, once, properly

BG2's tilemap is 32 columns -- 256 px, exactly the screen -- and acts as a
circular buffer over a city far larger than it. Scrolling therefore has to
rewrite the column about to appear at the LEADING edge, and because 32 columns
wrap onto themselves that same column is still on screen at the TRAILING edge.
One column has to hold two different contents in the same frame. It cannot.

That single geometry produced every seam chased here:

- **trailing edge** -- the game rewrites a column still visible behind you, so
  the incoming content flashes in at the far side;
- **leading edge** -- a column becomes visible before the game rewrites it, so
  it briefly still holds the wrapped content from 256 px away.

Vertically there is no such problem: 32 rows against 224 visible lines leaves 4
spare rows to stage into, and the game rewrites a row before exposing it.
Measured over 113 frames of downward scrolling: median 0.0%, worst 12.8%,
against 24-28% spikes horizontally.

On hardware the guest's outermost columns sat in CRT overscan and were never
seen. The game is built on that assumption.

## What the code does now

**Passe-partout** (`kPassePartout = 8`, `ws_passepartout()` in `src/main.c`):
the guest's outermost tile column is never displayed. The host map -- which
draws the same terrain from WRAM -- covers 8 px on each side permanently, so
the fault is gone by construction rather than repaired per frame.
`SC_PASSEPARTOUT=0` restores the guest's own edge columns.

`ws_fix_scroll_seam()` still exists but stands down whenever the passe-partout
is on, which is the default. It is kept only as the fallback that switch
selects.

Checked before cropping anything, since the HUD lives near the edges: while the
map scrolls, columns 0-15 change 60-89% (the toolbar starts at x~16), columns
240-255 change 34-59%, and the top and bottom rows change too -- the status bar
is a panel inside the picture, not a band across the edge. An 8 px crop takes
map pixels only, at every edge.

### Measured result

Both covers are **exact**: 100.0% match against a correctly translated previous
frame, every frame, on both edges, across normal, fast and diagonal panning.
Reported from play as "map scrolling itself works flawlessly - best picture so
far".

## Open

### 1. RESOLVED -- the "left cover dip" was a measurement artifact

This section previously recorded the left cover as mismatching 11-34% with dips
to 67-78% every fourth frame, against a flat 100.0% on the right, and called
that an unexplained asymmetry. **It was my metric, not the picture.**

The band test compared an 8 px cover against a window shifted by the scroll
delta. Shifted by +2, `x0-7` reads `x2-9` -- and `x8, x9` are GUEST pixels, so
two of the eight columns were comparing host content against guest content
every frame. The right band never had the problem: shifted, `x248-255` reads
`x250-257`, and `x256+` is more host strip, so it stayed host-vs-host. That
asymmetry was the whole "puzzle".

Measured with a window that stays inside the cover (`x0-5` shifted by +2), the
left band is **100.0% on every frame**, exactly like the right.

The dips clustering on `fx=6` frames was the same artifact seen through the
fine-offset cycle, not a cell-boundary fault.

Two things were ruled out along the way and are worth keeping, because both
were plausible and both are now excluded:

- **The renderer.** `SC_HOST_MAP_DUMP` on consecutive frames shows it is
  cell-granular and exact -- frames 80->81 it does not move, 81->82 it jumps a
  full 8 px, matching at 100.0% in every region.
- **The compositor's sampling.** `SC_COMPOSE_DIAG=1` prints `sx`, `fx` and the
  combined origin per frame: it advances by exactly +2 every frame, with the
  cell step (`dsx=1`) landing precisely when `fx` returns to 0. No jump.

**Lesson for the next measurement:** a band metric that shifts its window must
keep that window inside the region it is judging. Otherwise it reports the
boundary between two correct things as a defect.

### 2. Roof overlay: the horizontal half is unverified

Building tops were drawn one PIXEL up-left instead of one CELL (`cell / 8`
where `cell` is 8). Fixed to `- cell`, and confirmed from play: "Roofs are fine
now". Both axes were moved, on the strength of the `-1,-1` in the format spec,
but only the vertical error was ever reported. If roofs ever look a tile too
far LEFT, that half is the suspect.

### 3. DONE -- upstream merged; one inherited clone defect

The nine local commits and upstream's sixteen merged with a single conflicted
file, `runner/src/snes/interp_bridge.c`, in four hunks -- all the
deadline-unwind machinery, where `s_lle_unwind_from_deadline` (ours) and
`s_lle_unwind_is_deadline` / `s_lle_next_unwind_is_deadline` (upstream) are two
answers to one question. Upstream's won: same behaviour on expiry, plus it
clears `s_lle_unwind_active` and the owner depth, which ours did not. Kept only
what is additive on our side -- the coverage hooks `src/main.c` wires, and
`SNESRECOMP_DEADLINE_DIAG`.

**Verified rendering-neutral**: 30 frames of scrolling gameplay from
`savestate_5` are byte-identical before and after, 0 pixels differing. All five
targets build; qualify PASS at 2000 and 6000.

Two build fixes were needed, both recorded in the merge commit: a stub for
`wlog_addr_note_direct()` (upstream's `snes.c` now logs direct WRAM writes
through a function in `cpu_state.c`, which this target deliberately excludes --
safe to stub because it is purely a diagnostic), and MSVC portability for
`__attribute__((constructor))`, which MSVC rejects outright.

Submodule now on an earlier host branch of `blackerking/snesrecomp` (since merged).

#### Inherited: a nested submodule with no URL

`snesrecomp/.gitmodules` declares `lib/retcomm-rbengine` with a `path` and a
`branch` but **no `url`**, so `git clone --recurse-submodules` stops with
"No url found for submodule path". Identical on `origin/main`, so it is
upstream's defect, not the merge's -- but the pre-merge pointer did not carry
that gitlink, so this is a regression in clone-ability that arrived with it.

The outer submodule still checks out and the build is unaffected; only the
recursive clone errors. Filed as
[#41](https://github.com/mstan/snesrecomp/issues/41). If it is fixed by
supplying the URL, do not guess it -- `lib/recomp-net` points at
TechnicallyComputers, but that is an inference, not knowledge.

### 4. Filed upstream, still open

- **#30** -- the new renderer drops background rectangles on halved colour math
  when extra space is non-zero. Worked around by selecting the legacy renderer
  when `PPU_halfColor && PPU_addSubscreen`.
- **#31** -- overlay capture removes OBJ from the game render but never exports
  it. No workaround; sprites cannot be borrowed, only deleted.
- **#39** -- AOT and interpreter disagree on the abs,X/abs,Y read page-cross
  penalty. Tier accuracy only; results are identical.
- **#40** -- `__attribute__((constructor))` in `interp_bridge.c` does not
  compile on MSVC. Fixed on our fork; the fix is offered in the issue.
- **#41** -- `lib/retcomm-rbengine` has no `url` in `.gitmodules`, so recursive
  clone fails.
- #29 was filed by me and is **closed**: its headline measurement was the
  animated water, not a rendering fault. See the correction in
  `docs/upstream/ISSUE_scroll_tile_band.md`.

## Dead ends -- measured, do not retry

### Repairing the seam by translating previous-frame pixels

Worked, and is what `SC_PASSEPARTOUT=0` still selects: exact edges, median 0.0%
peak 0.0%. But it translates COMPOSED pixels, so every screen-fixed layer
inside its band moves with the map. That single property caused three separate
reports:

- the **cloned cursor** in the strip;
- the **ghosting HUD**, once the vertical block was widened to 16 rows across
  the whole guest width, dragging the status bar along;
- and it is why the repair could not simply be narrowed.

**Skipping pixels that look static** was tried as a cure and cost more than it
saved: a fast pan went from 0.0% back to **13.2%**, because genuinely wrong
pixels that happen to match the previous frame get skipped too. Screen-fixed
content cannot be separated from map content by inspecting colours after
compositing.

### Re-rendering the line with the old tilemap column

The principled fix for cloning: keep each rewritten tilemap entry's previous
value, swap it back, render the line again into a scratch surface, take the
strip from that. Sprites and HUD are drawn at their current positions, so in
principle nothing can clone.

Tried twice, worse both times:

| | translate | re-render v1 | v2 |
|---|---|---|---|
| fast right, left 16 | **1.5%** | 4.0% | 4.0% |
| diagonal, left 16 | **1.8%** | 10.9% | 8.1% |
| static pixels moved | **3.9%** | 14.5% | 10.4% |

v2 fixed the two faults v1 had -- gate evaluated once at vblank and carried, and
detection moved to the top of the frame so the first affected frame also has a
scratch -- and still lost.

**The render pass itself is exonerated.** `SC_PASS_DIAG=1` renders every line
twice and diffs: **0 of 100352 pixels differ**, every frame, even as the third
pass of the frame. An earlier version of that diagnostic compared two EXTRA
passes with each other -- neither of them the first -- and so could not have
caught what it was built to catch. So the fault was in the tilemap swap
bookkeeping, and a third attempt should dump the scratch beside the main frame
and confirm they differ ONLY in the swapped columns before assuming anything.

### The upper-tile table address

An earlier note recorded "0 drawn, 31360 skipped", cell ids `0x00..0x25`, and a
table with entries only at `0xf9..0x3ff`, and concluded the table address was
wrong. None of it reproduces. `SC_ROOF_DIAG=1` measures **1017 of 20000**
overlay tiles drawn, cell ids **0..630**, entries **120..942**. The lookup was
always working; the fault was the draw position.

## Diagnostics

| switch | what it does |
|---|---|
| `SC_PASSEPARTOUT=0` | show the guest's own edge columns; re-enables the repair |
| `SC_SEAM_FIX=0` | disable the repair (only reachable with the above) |
| `SC_SEAM_FIX_V=1` | re-enable the vertical repair (off: it dragged the HUD) |
| `SC_SEAM_LEAD_LT=1` | re-enable the left/top leading covers (off: they paint over the HUD) |
| `SC_PASS_DIAG=1` | render each line twice and diff extra pass vs MAIN render |
| `SC_ROOF_DIAG=1` | overlay-pass counts: drawn/skipped, cell id and table ranges |
| `SC_DUMP_DIR=<dir> SC_DUMP_INTERVAL=1` | record frames -- **works in interactive play now**, not just `--qualify` |

That last one matters more than it looks. It used to work only in
`run_qualification()`, so a capture session from play silently recorded
nothing -- and these defects only show while the map is moving, so they cannot
be caught in a screenshot. Every diagnosis here that needed the user's own city
depended on it.

## GOAL -- move the slow work off the emulated CPU, via fiber + HLE

The mechanism already exists and is documented in `src/sc_hle.c`: routines
declared `hle_func` in the recompiler config are replaced by native C. Today
exactly one is -- `00:930d`, wait-for-vblank.

Two constraints shape everything:

- **HLE is AOT-only.** `sc_hle.c` is "shared by every AOT-linked target";
  `UrbanRecomp`, the interp816 build actually played, never sees it.
- **Compiled bodies only run under `SC_FIBER=1`.** Without it the AOT target is
  a pure interpreter and reports `bounces=0`. It is guarded to the US ROM by
  fingerprint, so other regions stay playable on the interpreter.

### Where the time actually goes (measured)

`SC_INTERP_PROFILE=1` buckets interpreted PCs by 256-byte page. With
`SC_FIBER=1` over 900 frames of boot -- `bounces=5263` against
`interp_steps=2,790,629`, so the interpreter still carries almost everything:

```
00:91xx   45.3%      00:80xx    6.6%
05:93xx   27.7%      00:90xx    3.8%
00:92xx   16.2%      rest      <1%
```

Three pages are 89% of it, and both clusters are ideal HLE candidates:

- **`00:90xx`-`92xx`, 65.3% together -- the LC_LZ5 decompressor.** `00:90dd` is
  its entry and `00:926d` handles its source-bank crossing, so the hot pages
  are its inner loops. **A byte-exact native reimplementation already exists**
  in `tools/extract_graphics.py`, verified against a live run. See
  `docs/REFERENCE_map_format.md`.
- **`05:93xx`, 27.7% -- a block copy.** `05:9304` is `MVN $7f,$7e` moving 32768
  bytes `$7E8000` -> `$7F0000`, with a second at `05:9329`. That is a `memcpy`
  being executed one byte at a time on the emulated CPU.

**Caveat: this profile is boot**, which is decompression-heavy by nature. An
in-game profile will look different, and the simulation routines that matter
for "slow calculation" during play have NOT been profiled yet. Do that before
choosing what to HLE second.

### FIXED: save state + fiber hung

`load_state()` restores `g_snes` and the INTERP816 cpu. It knows nothing about
`sc_fiberdrive.c`'s `static CpuState s_cpu`, which is what the fiber
actually executes -- and `ScFiberDrive_Init()` runs during env parsing,
long before any state is loaded, pinning the 65816 reset contract (PB=0, DB=0,
D=0, S=$01ff, 8-bit A/index, resume at the reset vector).

So the fiber ran BOOT registers over MID-GAME WRAM.

`ScFiberDrive_AdoptInterpState()` now copies the architectural registers
across after a load and republishes the resume PC. Save state + fiber passes,
and the tier ratio in-game is far healthier than at boot: bounces=51732 against
interp_steps=1,302,787, versus 5263 against 2,790,629.

### Still open: a later hang, and what the in-game profile really shows

The same run still trips the opcode guard at frame 529 (was 60), with
`[frame] bridge bailed 31 frames running at frame 5671 (resume=00930D)`.

The in-game profile is dominated by `00:93xx` at 90.8% of 68M interpreted
opcodes -- and that is **not** stray work to delete. `00:930d` is the
wait-for-vblank spin, and `recomp/bank00.cfg` disables its HLE deliberately:

> Disabled for the run_loop frame model. The HLE existed for the FIBER design,
> where it was the only way to hand a frame back from arbitrary call depth. The
> bridge detects the same wait itself via yield_pc (00:9311 on $b9), but only
> in INTERPRETED code -- an HLE'd 00:930d in a compiled body returns
> immediately and the frame is never paced.

So the spin IS the frame-pacing seam. Enabling `hle_func 930d` would break
pacing, and `g_sc_yield_to_host` is NULL anyway -- nothing sets it.

That said, ~123k interpreted opcodes per frame spent busy-waiting is real
wall-clock waste even when it is functionally correct. The tractable idea is to
short-circuit the spin in the INTERPRETER -- recognise the wait on `$b9` at
00:9311 and advance to the NMI instead of interpreting the loop -- which is the
same shape as `sc_advance_until_input_ready()` for the input latch. That keeps
pacing while removing the opcodes.

### Superseded blocker note (kept for the isolation table)

Loading a save state while the fiber tier is active hangs the guest. Isolated:

| | result |
|---|---|
| AOT + save state, no fiber | PASS |
| AOT + fiber, no save state | PASS |
| **AOT + fiber + save state** | **hang at `$05935A`** |

`qualify: opcode guard tripped at frame 60 (hang/runaway)`, with
`[interp_cap] entry=$0080B2 last=$05935A op=$D0 m=1 x=0 db=$03 sp=$1FF5`. `$D0`
is `BNE`, so it is spinning in a tight loop in the `05:93xx` block-copy region.

**This matters beyond itself: it blocks in-game profiling**, because the only
cheap way into gameplay is a save state. The boot profile below is therefore
still the only valid one.

A profile taken during the hang shows `05:93xx` at 97.7% of 62M interpreted
opcodes. That is the runaway loop, NOT gameplay, and must not be used to choose
HLE targets.

Likely area: the save state restores guest CPU and WRAM, but the fiber tier
carries its own resume state (`s_lle_resume_pc24`, the fiber stack, the paired
return context) which the restore does not reconcile.

### Order of work

1. Fix save state + fiber, or find another route into gameplay -- until then
   no in-game profile is possible and HLE targets can only be chosen from boot.
2. Profile in-game.
3. HLE the decompressor -- highest measured share at boot, and the native code
   already exists and is byte-exact.
4. HLE the two `MVN` block copies -- trivial, and 27.7% at boot.
5. Map generation (below) is the candidate that unlocks *changing* generation
   rather than only speeding it up.

## OPEN -- map generation on decompiled code

Goal: generate maps natively rather than by running guest code, so generation
can be CHANGED -- larger maps, new terrain rules, chosen seeds -- instead of
only replayed. Started 2026-08-30 in `src/sc_mapgen.c`; it compiles and is
in the build, but nothing is wired in and **nothing is verified**.

### Done and VERIFIED: the PRNG

`00:824f`, transcribed with its disassembly quoted in the source, and checked
against the running guest: **14 of 14 sampled state transitions reproduced
exactly**, each one step apart. On 32-bit state that is conclusive.

The check has power, which matters more than the pass: dropping the carry
chaining between the two `ADC`s -- the easy mistake -- scores 8 of 14. It
agrees most of the time, which is exactly how that bug would survive casual
testing.

**How it is verified, since neither obvious hook works.** The interp816 core
never calls `interp816_opcode_hook`, and `interp_bridge.c`, which owns
`g_interp_bridge_pc_hook`, is not compiled into the main target. Both were
wired up and produced no output at all. `SC_MAPGEN_VERIFY=1` instead samples
`$59`/`$5b` once per frame; because the state is 32 bits, a correct step joins
consecutive samples in a few iterations and a wrong one essentially never does.

### Decompiled, NOT verified: the seeding

`03:d840`. Its entry carry is still unknown and is a parameter in the C rather
than a guess -- the `ROL` chain and the `ADC #$1238` both consume it.

### The scope, which is small

The generator is about 250 instructions in six routines, all reachable from
`01:f1ed`:

```
01:f22c   86 instructions      01:f311   45
01:f444   65                   01:f5b9   24
01:f3a3   18                   01:f380   14
```

`02:923f` is the DMA upload side, not generation -- a native generator writes
cells directly and does not need it.

### Verification comes first, and is the whole game

Nothing above is worth anything until a seed produces an identical map. The
map is fully determined by the three bytes `$0b27`-`$0b29` (`03:d873` copies
them to `$0b2a`-`$0b2c` immediately after generating, i.e. the game treats them
as the map's identity), so a seed/map pair is a complete test case.

1. **Trace the guest's PRNG.** `g_interp_bridge_pc_hook` already fires per
   interpreted opcode, so watching for PC == `$00824f` and recording
   `$59`/`$5b`/`$5d` gives the reference stream.
2. **Settle the entry carry.** `03:d840`'s `ROL` chain and its `ADC #$1238`
   both consume the caller's carry, which the disassembly cannot show. It is a
   PARAMETER in the C, not a guess, precisely so the trace can decide it.
3. **Capture a golden map**: dump `$7E0200` (12000 cells) with its seed bytes.
4. **Compare per routine.** A whole-map mismatch does not say which of six is
   wrong.

Two traps this project has already paid for apply directly: use `dis_mx.py`,
never `dis65816.py`, which does not track SEP/REP and mis-sizes operands after
a width change; and do not trust a harness-side number as if it came from the
emulator -- the audio work below measured the harness twice before noticing.

## OPEN -- audio desynchronises, and has since the project started

Reported from play as long-standing. Investigated 2026-08-30 and **not solved**.
What follows is mostly a record of two wrong measurements, because both are
easy to repeat.

### `--qualify` cannot measure this. Twice fooled.

**First attempt.** `--qualify` prints `audio_samples`, and the rate came out at
533.906 samples/frame across 2000- and 6000-frame runs, stable to three
decimals. That sits on 60.000 Hz rather than the SNES's 60.0988 (533.122), so
it looked like a +0.147% drift -- about a second every eleven minutes, the
right order for the symptom.

It was the harness. `run_qualification()` drains a hardcoded 534 samples per
frame (`if (available >= 534) dsp_getSamples(dsp, audio_buf, 534)`).

**Second attempt.** `SC_APU_DIAG=1` prints a per-frame ledger of what the DSP
actually wrote, which looked like the real thing: 533.908 samples/frame, with
`avail` steady at ~959.

Also the harness. `dsp_getSamples()` consumes a fixed 534 and the SPC is cycled
to supply them, so **the consumer dictates production** and the number just
echoes the drain. Proved by halving `kInterpApuPerMaster` and re-measuring:
533.913, i.e. no effect at all.

**So: no headless measurement of the audio rate is trustworthy.** The rate has
to be measured in the interactive path, where consumption is
`audio.freq / 60.0988` in the SDL loop and the SPC is cycled by the audio
callback.

### Still true, and still suspicious

Three copies of the same ratio use 60.0 where an NTSC frame is 60.0988:

```
common_rtl.c:973       kApuPerMaster       = (32040*32) / (1364*262*60.0)
interp_bridge.c:38     kInterpApuPerMaster = (32040*32) / (1364*262*60.0)
snes.c:25              apuCyclesPerMaster  = (32040*32) / (1364*262*60.0)   <- UNUSED
```

That makes the master clock 21,442,080 instead of 21,477,272 -- the constant is
0.1647% fast. Upstream `origin/main` has them identically, so it is not ours.

**They were corrected and the change reverted**, because nothing could be shown
to change: the headless rate is harness-dictated, so there was no way to
demonstrate a benefit, and shipping unverified timing changes is how the
left/top covers went wrong. The arithmetic is still wrong and worth fixing --
but only alongside a measurement that can see the difference.

`snes.c`'s copy is referenced by nothing at all; changing it first cost a build
to discover.

### How to actually measure it

Instrument the interactive path, not `--qualify`: log the audio callback's
consumption against the DSP's production over a real session, with
`SC_DUMP_DIR` running so video frames are timestamped alongside. The question
to answer first is whether production and consumption differ at all in that
path -- everything above leaves it genuinely unknown.

## OPEN -- title: the publisher and title building parts move wrongly

Reported from play 2026-08-27, straight after the light-row fix below. Not yet
investigated at all; this entry is the report and a starting point, nothing
more.

The title animates its logo out of building pieces. Those pieces are moving
wrongly. Which way is wrong -- speed, direction, offset, or only in the
widescreen margins -- is NOT recorded, so establish that first from a capture
rather than guessing.

Where to start:

1. `SC_DUMP_DIR=<dir> SC_DUMP_INTERVAL=1` while the title plays, then measure
   the pieces frame to frame. Interactive capture works now, so a real session
   can be recorded and analysed.
2. `SC_LAYER_MASK` to find out WHAT draws them -- bit0 BG1, bit1 BG2, bit2 BG3,
   bit3 BG4, bit4 OBJ. The light row is OBJ; if the building pieces are OBJ too,
   `widen_title_lights()` is the only host code touching title sprites and is
   the first suspect. If they are a background, the margin/clamp path is.
3. Compare against `SC_WIDESCREEN=0`. If the motion is correct at authentic
   width, it is ours; if it is wrong there too, it is the guest or the device
   model, and none of the widescreen code is implicated.

Note the light-row bug found the same day was a host bug in title sprite
handling, so step 2 is worth doing before anything else. But do not assume it:
the two may be unrelated.

## Title lights: fixed 2026-08-27

Reported from play: on the title, the widescreen light row was missing on the
LEFT for the first frames, appearing only once the title started to move.

Measured with `SC_LAYER_MASK=0x10` (sprites only): **0 px in the left margin
against 1293 in the right**, on every title frame. It never worked; what shows
once the title moves is the game's own sprites entering the margin.

`widen_title_lights()` measured the row's extent over every OAM member sharing
the row's Y/tile/attribute -- including one parked off-screen at **x = -255**.
That made `lo = -255`, so the leftward loop started at `lo - pitch = -319` and
its first condition (`x >= -extra - pitch`) was already false. It placed
nothing, ever: `placed L=0 R=2`.

The extent is now measured only over members the authentic viewport shows
(`-16 < x < 256`), which gives `lo = 1` and `placed L=2 R=2`. Left margin
renders 1296 px against the right's 1293.

The left OAM hints (`PpuWsSetOamLeftHints`, from the upstream merge) were wired
up at the same time, mirroring the right. They turned out NOT to be the cause --
strict versus permissive decode is 0 differing pixels on four save states -- but
they are kept: our own placed slots are now claimed explicitly on both sides
rather than relying on the permissive default.

`SC_LIGHTS_DIAG=1` prints the row the extender found: member count, y, lo, hi,
pitch, and how many sprites it placed each side.

## OPEN -- the mapgen turbo boosts the wrong thing

Reported from play: the mapgen turbo appears to speed up the SIMULATION on the
normal map rather than map generation.

Measured, and the mechanism is visible even though the effect is not
reproducible headlessly:

- The boost is `frames_this_iter = generating ? s_mapgen_turbo : ...` and lives
  ONLY in the SDL loop. `--qualify` counts the flag but never multiplies, so a
  headless run can confirm when the flag is set and nothing about how it feels.
- `s_generating` is bounded by two PCs: set at `03:d862`, cleared at `03:d871`.
- **Starting a map trips it.** On savestate_6 (press B to start), the flag is
  active for **71 frames after the press**, with `trigger_hits=4`. In the
  interactive loop those 71 frames run at 16x -- about a second of visibly
  fast simulation right as the city appears, which is exactly what was
  reported.
- During generation proper the flag does work: 638 boosted frames of 700 on
  savestate_3 + Up.

So the pair does not bound what it claims to. `03:d862` is the PRNG call inside
`03:d840`'s seeding loop, and that path is evidently entered when a map is
started, not only when one is generated.

Two candidate fixes, neither implemented because neither can be verified
without interactive play:

1. Also clear `s_generating` when the screen index `$14` changes to the city --
   the boost has no business surviving a screen transition.
2. Cap the boost with a frame budget, so a stuck flag degrades to a brief
   speed-up rather than an unbounded one.

Related: generation is genuinely slow even when boosted. On savestate_3 + Up it
was still running at frame 700 (`final_pc=01:f53b`, inside the fitting pass)
with 638 frames boosted at 16x. The turbo is working and generation is simply
long -- worth knowing before raising the factor further.

## Also open, pre-existing

The host strip moves a different distance from the guest on ~25 of 106 frames
during fast horizontal pan. Not caused by the seam work -- disabling that
correction gives 27. Probably the "clearly apart for a split second" reported
early on.

## Title margins: the parked building on BG1

**Open. One approach tried and reverted -- do not repeat it.**

Reported from play: an animated tile carrying the title lettering rides in
with a building, reaches the left edge and parks there instead of leaving; the
building shows the publisher's name beneath it and departs normally while the lettering stays.

Located by clamping one layer at a time and rendering the margin as text:

    clamp BG2   the letter shapes go, the building stays
    clamp BG3   nothing changes
    clamp BG1   the building and its rows of windows go
    clamp all   the margin is empty

So the building is on BG1 and the lettering on BG2.

### What was tried, and why it is wrong

Clamping BG1 on the title ($14 == 0x01). It does remove the parked building --
and it removes EVERY OTHER BUILDING FROM BOTH MARGINS with it, because BG1's
off-screen columns carry the real skyline as well as the parked object.
Reported immediately: "rendering of all buildings are only inside the normal
view, the widescreen got nothing". Reverted.

The measurements that made it look right were all confirming: the building was
gone, nothing else regressed, the wood still worked. None of them asked what
ELSE the margin lost, because a non-black pixel count cannot tell a wanted
building from an unwanted one.

### Why this is hard

Hardware shows nothing beyond x=256, so the game is free to leave anything it
likes in the columns past the window -- and does. Widescreen deliberately shows
those columns. There is no property of a tile that says "scenery" or "parked";
both are just map entries. A whole-layer clamp cannot express the difference,
which is why it takes the wanted content with it.

### Second attempt, also reverted: blank the logo tiles in the margin columns

The layer attribution above is right and the tile ranges are real:

    stuck at the left   margin columns hold 003..01a in the outer four
    mid-screen          margin columns hold only 06c..083, no low tiles
    full map at hs=0    logo occupies 001..023, buildings start at 024

Blanking BG1 cells below 0x024 in the eight tile columns each margin reads
LOOKS right in a still: on the stuck save state the glyphs are replaced by the
scenery behind them, 814 margin pixels change, and the visible 256 px is
byte-identical; on the mid-screen state nothing changes at all.

In motion it is WORSE than the artifact it removes. Reverted.

The still frames could not have shown that, and I knew it -- the handover note
even said "worth confirming in motion". Two frames from two save states cannot
see a cell being blanked and unblanked as the logo scrolls across the boundary,
and that is where this lives: the suppression is recomputed every frame from a
moving hScroll, so a column enters and leaves the blanked set as the logo
crosses it.

If this is tried again, the test has to be a MOVING capture through the moment
the logo crosses the left edge, compared frame by frame -- not a pair of
stills, however carefully measured.

### What might work

  - A COLUMN rule rather than a layer rule: find the columns the game parks in
    (they should be stable across the title sequence) and blank only those.
  - A ROW rule: the parked object sits at a known band of scanlines; the
    skyline that should wrap may occupy a different one.
  - Accept it. The building leaving the view and reappearing is a cost of
    seeing past the authentic edge, and the alternative just tried is worse.

Whatever is attempted, the check is NOT "is the artifact gone" -- it is "what
does the margin still contain". Render it and look.
