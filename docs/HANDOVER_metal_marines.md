# Handover to Metal Marines (and any other snesrecomp game repo)

Problems this project solved the expensive way, written down so a sibling repo
does not pay for them twice. Nothing here is SimCity-specific unless it says
so. Ordered by how much time each one cost.

Metal Marines already hit #1 independently, which is why this file exists.

---

## 1. The joypad bit order — `input*_currentState` is SERIAL order

**Cost here: three wrong diagnoses across two sessions, eleven ROM patches
written and then deleted, and an invalid upstream issue.**

The runner's `input1_currentState` is **not** the `$4218`/`$4219` hardware
layout. It is the serial order the pad shifts out of `$4016`, LSB first:

```c
kPad_B = 0x0001, kPad_Y = 0x0002, kPad_Select = 0x0004, kPad_Start = 0x0008,
kPad_Up = 0x0010, kPad_Down = 0x0020, kPad_Left = 0x0040, kPad_Right = 0x0080,
kPad_A = 0x0100, kPad_X = 0x0200, kPad_L = 0x0400, kPad_R = 0x0800,
```

`snes.c` reverses all 16 bits and splits the result, which lands the real
hardware layout: `$4218` = A,X,L,R + pad ID, `$4219` = B,Y,Select,Start,
Up,Down,Left,Right. **With the runner unmodified.**

Use the naive hardware order and every button is wrong in a way that looks
like a *game* bug, because the d-pad still half-works. This project blamed the
ROM (and patched it), then blamed the runner (and patched that, and filed
[issue #14]), before finding the host was wrong both times.
`mstan/snesrecomp` [PR #17] added a regression test and is merged.

Second-order damage worth knowing about: scripted `--input` masks written in
the wrong convention put B/Y/Select/Start on bits 12-15, **which are not
button bits at all**. Those four are then silently never pressed. It
invalidated a coverage conclusion here ("these save states cannot be driven
headless") that had stood for weeks.

**Check first:** hold one direction on a fixed save state and watch a cursor
variable move. Ten seconds, and it settles the whole question.

---

## 2. Do not write a bespoke per-opcode host loop

**Cost here: still unpaid — it is currently blocking the AOT migration.**

Writing your own `main.c` with a per-opcode `runOpcode` + advance-devices loop
is the obvious way to bring a game up, and it works beautifully right up until
you want to run compiled code. Then:

- **Nothing else drives your PPU.** `interp_bridge_run_loop` advances the APU
  and `master_cycles` but never the beam; a fiber freezes it entirely. Any ROM
  loop that waits on a beam-derived register (`$4212` auto-joypad-busy is the
  classic) deadlocks. SimCity's *boot path* has one, before the first vblank
  wait, so no amount of AOT coverage makes a compiled-entry design boot.
- `interp816_opcode_hook` looks like the escape hatch. It is declared in
  `interp816.h`, defined as a no-op in `interp_bridge.c`, and **never called
  anywhere in the runner**. Dead extension point.
- You silently opt out of everything hanging off `RtlRunFrame`: the mod
  frame/APU callbacks (`snes_mod_runtime_frame_tick_c`), MSU-1 packs,
  widescreen, netplay.
- And of the CMake seam: **SDL3 is the default only for hosts that include
  `runner/runner.cmake` and call `snesrecomp_target_sdl(<target>)`**. Roll
  your own `find_package(SDL2)` and you are on SDL2 forever without noticing.

**Recommendation:** start from `runner/src/desktop/mmx23_host_main.inc` and
the runner's frame model. If you have already gone the other way, keep the
per-opcode loop as a differential oracle — it is genuinely better for finding
accuracy bugs (it is how the missing-HDMA bug was found here) — but do not
make it the only path.

---

## 3. Exit-M/X coverage: measure it, and check the measurement

The analyzer cannot prove exit widths through mutually recursive dispatch, and
an unpublished exit truncates **every** caller transitively. It is usually the
single biggest coverage item. Three game-agnostic tools live in `tools/` here
and port unchanged:

| tool | question |
|---|---|
| `mx_exit_report.py` | what widths do unproven callees return in? |
| `mx_exit_check.py` | do the analyzer's *published* exits match the machine? |
| `mx_exit_propose.py` | which measurements are solid enough to emit as a directive? |
| `gen_align_check.py` | does the emitted C decode on the boundaries the CPU used? |

They need `SC_MX_BITMAP` — the executed-PC bitmap split four ways by the live
`(m,x)` flags, four planes of 64x4096. Roughly 20 lines in your host's opcode
loop; copy it from `src/main.c`.

Hard-won details, each of which cost a wrong answer here:

- **Key the callee variant on the width at the CALL SITE**, not at the callee's
  target address. `JSR`/`JSL` change neither M nor X, so the site's own width
  *is* that site's entry variant; the target address unions every caller.
  Getting this wrong produced "40 mismatches", then "26 systematic mismatches",
  both of which were pure artefact. Correctly keyed: 1,584 sites, 1,584 agree.
- **Read the exit at the routine's own `RTS`/`RTL`**, not at callers' return
  addresses, when the call sites are indirect (`JSR (abs,X)`) — the return
  address is fixed but the *target* is chosen at runtime, so those widths union
  over whichever handler the dispatch picked.
- **Bound a routine by the next entry address**, not by its node's `max_pc24`.
  `max_pc24` both swallows nested routines and stops short of a truncated
  one's real return. Then follow executed unconditional jumps out of the
  extent — a routine can `JMP` into a shared tail and return from there.
- **"Split" and "split, and we have seen all of it" are different claims.** A
  declared exit *set* missing one real width is worse than no set: the decoder
  forks the post-call continuation once per declared width, so an omitted
  width is a continuation never decoded.
- Derive directives from one half of your recordings and verify on the other.
  Checking a measurement against its own source data proves nothing.

Upstream gained `exit_mx_set <addr> <entry MmXn> <exit MmXn>[,...]` for
callees returning several widths from one entry variant (branch
`feat-exit-mx-set`; `exit_mx_at` broadcasts one width to every variant and
`exit_mx_at_per_variant` gives one per variant — neither can express a set).

---

## 4. Coverage hunting: two knobs that pull opposite ways

Measured here, both counter-intuitive:

- **Run length.** Time-gated code needs long runs. An in-game year in SimCity
  is 9,600 frames; every hunt was 2,600 and therefore *structurally incapable*
  of reaching the annual budget path. Work out what a path costs in frames
  before concluding it is unreachable.
- **Idle gaps.** UI-state code needs the opposite. Twelve runs with
  near-continuous input reached a dialog gate **0/12**; twelve with sparse
  input and 300-600 frame gaps reached it **3/12**. Holding buttons suppresses
  the dialogs you are hunting.

And the one that cost the most: **check whether the path is reachable from the
state you are driving before adding more runs.** About a hundred runs here
chased a branch behind a locked menu entry, from save states where it was
locked. A human who knew the game unlocked it in ten seconds.

## 5. `SC_FREEZE`-style state pinning contaminates measurements

Holding a WRAM byte at a value the ROM never holds drives execution into
states no code path produces. Measured: adding 20 frozen runs to a 99-run
union turned a clean check (1,205/1,205) into 2 mismatches, produced a false
decode-desync report, and recorded executed PCs in bank `$18` — outside the
512KB ROM image, i.e. the CPU running off into open bus. Freeze is a fine
instrument for "is byte X gating behaviour Y"; it is not a way to manufacture
coverage. Keep such runs out of any union you draw conclusions from.

## 6. Smaller traps

- **A cfg `func` with no `entry_mx_at` defaults to `M1X1`.** If the routine is
  only ever entered at another width, that variant decodes garbage — and it
  will be `aot_eligible` with an empty `reasons` list, i.e. it looks clean.
  Found here at `00:926d`: a three-byte `LDY #$0000` read as two bytes, decode
  resuming mid-instruction. `entry_mx_at` must appear **before** the `func`
  line; `cfg_loader` applies it as the func is parsed, in a single pass.
- **A disassembler that does not track SEP/REP will desynchronise**, and one
  wrong immediate corrupts everything after it. Use a width-tracking one
  (`tools/dis_mx.py` here) and *read the execution marks* — a `*`-free block
  never ran and is not evidence.
- **Inline-argument callees** (routines that consume bytes after their `JSR`
  and adjust their return address) were miscompiled framework-wide until
  `mstan/snesrecomp` PR #19. Fixed upstream now, but if you see a caller
  resuming at `call + size` where the bitmap says the next executed byte is
  further on, that is the shape.
- **MSVC has no `__attribute__((weak))`**, so `cpu_state.c`'s fallback guard
  table collides. Worked around here with `/FORCE:MULTIPLE`;
  `__declspec(selectany)` is the real fix.
- **Upstream moves.** `runner/src/snes/tier2_capture.c` was added after PR #17
  merged and `interp_bridge.c` now calls into it, so every target compiling
  `interp_bridge.c` needs it or the link fails on
  `tier2_capture_manifest_path`.

## 7. Method notes that generalise

- **Attribution by containment beats attribution by difference.**
- **One save state agreeing with a hypothesis is not evidence** if another
  variable explains it equally well. A WRAM address here was called the
  nuclear-plant count when it was the airport count.
- **A plausible correlation is not a finding.** A three-bit field next to a
  No-Disasters cheat got written up as the disaster system and was wrong;
  proximity is not evidence.
- **When the person who knows the game contradicts your static reading, the
  reading is wrong.** That held every time it came up here, including the case
  that unblocked 5,663 instructions.

---

## 8. SDL3: every incompatibility we hit was silent

If you take the runner's SDL3 default (`runner.cmake` +
`snesrecomp_target_sdl()`), budget for this. Three defects, and **all three had
the same signature: the code runs, every API returns success, and nothing
appears.**

| symptom | cause |
|---|---|
| window never opens, `SDL_GetError()` empty | `SDL_Init` returns **true** on success in SDL3, `0` in SDL2 -- `!= 0` reads success as failure |
| window opens, audio plays, **black screen** | SDL3 defaults textures to blending; a framebuffer that carries no alpha (`A=0`) renders fully transparent. Fix: `SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE)` |
| overlay/menu toggles but is invisible | SDL3 renderer rects are `SDL_FRect` (float), not `SDL_Rect` (int). `SDL_ENABLE_OLD_NAMES` keeps the *names*, not the *signatures*, so four ints are read as two floats and the rect lands off-screen |

Two more that only bite when you touch them:

- `SDL_RenderPresent` returns `void` on SDL2 and `bool` on SDL3 -- a shared
  "check the return" macro will not compile on both.
- `SDL_GetRendererOutputSize` (SDL2) is `SDL_GetRenderOutputSize` (SDL3), and
  the old name does not alias.
- `SDL_RenderReadPixels` returns an `SDL_Surface *` rather than filling a
  buffer, and on at least one Windows backend **returns black regardless** --
  so any screenshot-based self-check silently reports an empty screen.

### The method point, which matters more than the list

**A headless test bar cannot verify a renderer change.** `--qualify`, WRAM
differentials and unit tests all passed on every one of the broken builds,
because none of them initialises video. Two defects reached a build reported as
"fully verified" through that gate.

What does work, and costs nothing: dump the *framebuffer* (not the renderer)
and compare it byte-for-byte between backends. It touches no SDL rendering API,
so it validates emulation and picture content independently of the backend, and
it would have caught the black screen immediately. Everything else needs a
human looking at the window -- which is how all three were actually found.
