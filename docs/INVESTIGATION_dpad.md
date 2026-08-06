# D-pad investigation (resolved)

Status: **fixed**. The D-pad now works on every screen tested: map
scrolling, the build cursor, the toolbar, in-game menus, the Information
panel, the mode-select (start) menu, Scenario Select, Save, Tax, the
Load/Save/Exit menu, the Map Select scenario-number picker, the
city-name-entry on-screen keyboard, the Select-game-level (Easy/Medium/
Hard) screen, the Comprehensive/Information map overlay (both scrolling
and the cursor), and the View screen (the watch icon). Confirmed by
interactive testing on every screen, and independently cross-checked
against real hardware-accurate emulation (bsnes) partway through the
investigation. Two known remaining gaps: the map's "fast travel" modifier
(X or Y held while moving, for a bigger/faster scroll jump) is not yet
fixed, and the View screen's D-pad now genuinely updates its underlying
WRAM state (confirmed live) but nothing visible changes on screen yet --
see "Open items" at the bottom for both.

## Root cause

The D-pad's held/edge state is correctly plumbed end-to-end by the shared
runner and the ROM's own shared edge-detector (`00:928f-92cb`, verified
against an independent bsnes trace instruction-for-instruction): it XORs
`$4218,X`/`$4219,X` against the previous frame, writes the "just pressed"
edge to `$0123,X` and a direct-page mirror at `$c9,X`/`$ca,X`, and writes
the raw held state to `$011b,X`/`$011c,X`. All of that is correct, and was
re-verified multiple times this session.

The bug is downstream, in the many places throughout the ROM that
*consume* this state. All of them follow the same broken pattern: they read
the **wrong half** of a 16-bit edge/held-state word. Concretely, real SNES
controller hardware guarantees the low nibble of `$011c`/`$ca` (the high
byte of the 16-bit word, mirroring hardware register `$4219`'s low nibble)
is always zero -- those are unconnected controller-port pins, on any real
SNES, for any game. Direction bits (Up/Down/Left/Right) actually live in
the low nibble of `$011b`/`$c9` (the *low* byte, mirroring `$4218`). Code
that checks the high byte for direction bits is checking bits that can
never be set, on this recomp or on real hardware -- confirmed both ways.

This is not a recomp bug. It reproduces identically on bsnes against the
unmodified ROM file, with no patches applied. It's a genuine defect in the
shipped cartridge, evidently never caught because the game is fully
playable via the mouse-equivalent point-and-click buttons (A/B/etc.)
without ever needing the D-pad.

## The fix

`src/main.c` patches the ROM image in memory at load time (game-specific,
never touches the shared snesrecomp runtime) to repoint each broken read at
the byte that actually holds the direction bits. Six distinct shapes of
the same underlying bug were found, each needing a slightly different
patch (plus one false trail worth noting):

1. **16-bit `LDA $011b`/`LDA $c9` (dp) followed by `AND #$0f00`, then an
   ASL/BCC or XBA/LSR ladder.** The ladder's shift calibration is tuned for
   testing bits 8-11, so simply changing the AND mask desyncs it from the
   ladder (confirmed by bitmap-diff: the "fixed" mask just falls through
   every ladder tap to the "no match" case). Instead, the fix shifts the
   *source address* back by one byte (`$011b`→`$011a`, `$c9`(dp)→`$c8`(dp)),
   which puts the real direction bits in the high byte where the
   unmodified ladder already expects them. 8 sites for the `$011b` form, 2
   for the `$c9`(dp) form.
2. **8-bit `LDA $ca` (dp) with various masks including bits 0-3** (the
   Tax-screen modal, several of the 20 handlers in the mode-select/
   list-menu dispatch table at `03:d255`, and the Map Select
   scenario-number picker at `03:d3e2`). Since these are single-byte
   loads (not 16-bit), the fix is simpler: just repoint the load at `$c9`
   directly. 2 sites (`02:a50c`, `02:ab1f` -- later found to be dead code,
   see below) + 1 site (`03:d33e`, mode-select) + 3 more sites across modes
   5 and 11 (Scenario Select / Save) + `03:d3e2` (Map Select).
3. **A gate excluding direction on purpose.** `02:a4ec` does `LDA $011b;
   AND #$fff0; BNE ...` -- deliberately testing "is any *non-direction*
   button held", a legitimate, correct idiom used elsewhere in the ROM (not
   part of the bug family above). This gate blocked Tax's already-fixed
   per-frame cursor-update code from running unless some other button was
   *also* held alongside direction -- confirmed live: Down+A moved the
   cursor, but Down alone did nothing, matching the gate exactly. Fixed by
   widening its mask from `#$fff0` to `#$ffff` so direction alone also
   satisfies it (one byte).
4. **16-bit `LDA $c9` (dp) followed by `AND #$0300`/`AND #$0200`, no
   ladder involved.** The Load/Save/Exit top-level menu (`00:d1b8`) and the
   Select-game-level screen (`03:d97b`) test bits 8-9 of the combined word
   directly (no ASL/BCC ladder to desync), so the same address-shift fix as
   variant 1 applies (`$c9`(dp)→`$c8`(dp)) with no extra care needed.
   `00:d1b8` fixes a symmetric Left/Right pair (`00:d1c4` decrements,
   `00:d1db` increments a shared selection-index byte at `$0421`, both
   sharing this one gate). Two neighboring checks at `00:d1aa`
   (`AND #$8000`, tests `$ca` bit 7 = A) and `00:d1b1` (`AND #$0040`, tests
   `$c9`'s own bit 6 = Y) already read real, valid bits and were left
   alone.
5. **Absolute (not direct-page) `LDA $0124` followed by `AND #$0f`.** The
   city-name-entry on-screen keyboard (`03:dad9`) reads the *absolute*
   high byte of the shared edge-detector's `$0123` mirror (`00:928f-92cb`'s
   16-bit `STA $0123,X` writes low byte to `$0123`, high byte to `$0124`)
   instead of the direct-page `$ca` mirror -- same hardware-zero-nibble
   region, different address entirely, so the earlier byte-pattern scans
   (which only looked for the direct-page form) missed it. Found via a
   fresh F1-bitmap-diff pass after every known `$c9`/`$ca`(dp) candidate
   came back unreached on this screen. Fix: repoint at `$0123` (absolute
   addressing is always 3 bytes regardless of M width, so nothing
   downstream shifts).
6. **Absolute (not direct-page) `LDA $011c` followed by `AND #$0f`, plain
   8-bit, no byte-shift needed.** The Comprehensive/Information map
   overlay has two chained sites in bank 2 -- `02:8525` (map scrolling) and
   `02:9f37` (the gate for a cursor-offset ladder) -- both reading the
   absolute mirror of the hardware-zero-nibble byte directly, so `AND
   #$0f` always comes back zero and each routine falls straight through
   its `BEQ` into a bare `RTS` before doing any work. Found via a fresh
   F1-bitmap-diff pass on this specific screen. A third site, `02:9f43`,
   sits just past `02:9f37`'s gate (once that gate is fixed and lets
   execution through) and re-reads the same dead byte to feed the actual
   cursor-offset ladder (`LSR A`/`BCC` testing each direction bit,
   incrementing or decrementing `$01eb,X`) -- this is why fixing `02:9f37`
   alone made scrolling work but left the cursor itself still frozen, and
   why `02:9f43` had to be found in a second, targeted capture after the
   first fix landed. A fourth site of the same shape, `01:f0d3`, sits in
   the shared bank-1 code and gates the View screen's direction-dispatch
   loop -- found the same way, once a separate rendering bug (see
   `docs/INVESTIGATION_hdma.md`) stopped masking whether this screen even
   responded to input at all. All four: repoint at `$011b`.
7. **A false trail worth noting:** Map Select's real selection variable
   turned out to be `$0b2d` (confirmed genuinely changing on direction
   presses via live tracing), but fixing the gate that reached it wasn't
   enough on its own -- the `$0b2d` sprite-position update runs
   unconditionally every frame regardless of whether real "focus" changed,
   which is what made it *look* like the mechanism worked while `03:d3e2`'s
   gate (variant 2) was still silently swallowing every direction press
   upstream. The lesson: a variable changing in response to input doesn't
   prove the *feature* works -- trace all the way to what actually reads
   that variable to decide the user-visible outcome (in this case, the
   sprite update ran either way, so the visible "cursor" that mattered was
   actually static hand-cursor artwork, not `$0b2d`'s target).

All patch sites, their exact file offsets, and the reasoning for each are
documented inline in `src/main.c` right where they're applied (search for
"D-pad fix").

## How each site was found

Two different techniques, used together:

- **Static byte-pattern scanning** for the exact instruction sequences
  above (`AD xx xx 29 00 0F`, `A5 C9 29 00 0F`, `A5 CA ...`), then manual
  disassembly of each hit to confirm the shape. This found most sites
  quickly but has a real false-positive risk: `dis65816.py` doesn't track
  SEP/REP width changes, so linear disassembly starting from an arbitrary
  byte offset can misalign across a flag change or an unrelated `RTL`
  boundary and decode garbage as if it were real code (hit this twice this
  session -- see "Pitfalls" below).
- **Live, interactive PC-reachability bitmap diffing** (see "Tools" below):
  run the same screen twice, once with a direction held and once without,
  and diff which ROM addresses were ever executed. This is what actually
  distinguished *live* candidate sites from coincidental byte matches that
  are never reached (`02:a50c`/`02:ab1f` looked identical to the working
  Tax fix but turned out to be dead code in that specific call context --
  the real, reachable Tax fix ended up being the `02:a4ec` gate, found via
  a live bsnes trace instead).
- **Live bsnes tracing** (user-captured, cycle-accurate real-hardware
  emulation) was decisive for the harder cases: it found the `03:d255`
  mode-select dispatch table, the `$0d67`-based Tax cursor mechanism, and
  the `02:a4ec` gate, none of which turned up from static scanning alone.
  It also served as independent, real-hardware confirmation that the bug
  is genuine ROM behavior, not a recomp artifact.

## Pitfalls hit along the way (useful if this pattern recurs elsewhere)

- **Raw 16-bit value scans for indirect jump-table references** (scanning
  every 2-byte window in the ROM for a target address's bytes) produce
  false positives from coincidental data bytes that happen to match --
  confirmed by disassembling the hit sites, which decoded as nonsense
  (matching known tile/price/scenario data table regions, not code).
- **`dis65816.py` misaligns across untracked SEP/REP flag changes.** Hit
  this at least three times this session (`01:ae1c`, `02:a500-a520`,
  `03:d3d8-d400`). When a decode looks suspicious (garbage opcodes,
  operand-length mismatches), re-derive alignment by hand from a known
  boundary (an `RTL`/`RTS`, or a byte offset independently verified via
  `tools/rawdump.py`-style raw hex) and track M/X width by hand across
  every `SEP`/`REP`.
- **A bitmap-diff "zero difference" result is only trustworthy with a
  positive control.** Every "nothing responds to direction here" finding
  in this investigation was paired with a same-context test of a
  known-working button (usually A) to confirm the diffing methodology
  itself was sound, not just silently failing to detect anything.
- **Live captures must be scoped to the exact frame window under test,
  or they're contaminated by earlier activity.** Reaching a target screen
  (e.g. navigating to Tax via the Information panel) itself exercises
  D-pad-handling code -- if that's included in the compared window, "zero
  diff" is meaningless. The `F1` in-session bitmap reset hotkey (below)
  exists specifically to solve this: press it once after arriving at the
  screen under test, before touching D-pad, so only what happens next gets
  captured.

## Tools produced during this investigation

- `SC_IO_TRACE` / `SC_PC_TRACE` / `SC_DEBUG` / `SC_DUMP_AT`+`SC_DUMP_PATH` /
  `SC_ADDR_TRACE` -- see the doc comment block above their declarations in
  `src/main.c`. `SC_ADDR_TRACE` also dumps the last up-to-128 executed PCs
  (ring buffer) on an address's first 3 hits, showing the real (possibly
  indirect) call path instead of requiring a static JSR/JSL scan.
- `SC_PC_BITMAP_BANK=<bank hex>|all` + `SC_PC_BITMAP_PATH=<file>` +
  `SC_PC_BITMAP_START=<frame>` -- records a 1-bit-per-address "was this PC
  ever executed" bitmap, either for one bank or (`all`) for every bank at
  once. Works in both `--qualify` and windowed/interactive mode (dumped at
  exit either way), which is what made the live-session workflow possible:
  launch, play to the screen under test, close the window, dump is on
  disk. Diff two runs' dumps bit-by-bit to find every address whose
  reachability differs between two input scenarios.
- **`F1` hotkey** (windowed mode only, active whenever
  `SC_PC_BITMAP_BANK` is set): resets the bitmap capture to empty without
  restarting the process. Press once after navigating to the screen under
  test, before providing the input being investigated, so the diff isn't
  contaminated by the navigation itself.
- `SC_LIVE_C9CA=1` (windowed mode): logs every change to
  `$011b`/`$c9`/`$ca`/`$01fb`/`$14` to stderr live, for eyeballing exactly
  what a real interactive session's WRAM state does frame-by-frame.
- `banks_seen` diagnostic line at qualify-exit: bitmask of every bank
  ($00-$3F) the CPU ever executed in during the run. Useful for narrowing
  which banks to bitmap-diff before doing the (slower) all-banks capture.
- Previous tools (`dis65816.py`, `dump_bank.py`, `ppm2png.py`,
  `ppmdiff.py`) still current; see "Pitfalls" above for `dis65816.py`'s
  known SEP/REP tracking gap.

## Fast travel: confirmed broken in this recomp, root cause not yet found

**Status: broken, contradicts a static trace -- unresolved, flagged for
re-examination.** A long live-testing session (cross-checked against
real-hardware-accurate bsnes running the same ROM) nailed down the
actual mechanism and a genuine, still-unexplained discrepancy:

- **Real behavior (bsnes, confirmed "working as intended")**: holding
  SNES Y **or** SNES A *simultaneously* while pressing a direction (not a
  toggle -- released Y/A immediately stops the effect) makes the map
  scroll genuinely faster, with the four directional arrow-cursor
  indicators visible throughout. SNES L/R have no visible effect either
  way.
- **This recomp**: holding the same buttons (keyboard A = SNES Y,
  keyboard X = SNES A, per this recomp's keymap -- see the controls table
  in README.md) shows the same arrow indicators, but the view **never
  moves at all** while held; only the game's normal edge-of-screen
  auto-scroll still works. Confirmed multiple times, unambiguously, not a
  toggle-vs-hold confusion.
- **The contradiction**: a careful, flag-tracked static trace of
  `01:8bd3`-`01:8c52` (the `$c5` reason-code dispatcher, see the WRAM
  table's `$00c5` entry in `docs/ROM_MAP.md`) found that holding Y
  *together* with a direction causes **every** branch in that dispatcher
  to fail to match, falling through to reason code `0` (a bare `RTS`,
  i.e. the dispatcher does nothing) -- specifically because Y's own check
  at `01:8be1` is gated behind `$01f5 == 0` (no direction currently
  held), so Y+direction skips it, and no other branch catches that
  combination either. This is the *same* ROM bytes bsnes executes, so if
  the trace is right, bsnes should show the same nothing-happens result
  it doesn't. **Conclusion: the trace has an error somewhere, or a wrong
  premise (e.g. this may not actually be the dispatcher gating main-map
  movement, despite `docs/ROM_MAP.md` describing bank 01 that way) --
  not yet resolved.**

Also found and ruled out separately: **SNES B/X** show the identical
arrow-indicators-no-movement symptom via a *different*, cleanly-traced
path (reason code `1`, `01:8d26`, calls the per-direction sprite routines
`b030`/`b166`/`b1f6`/`b2f9` directly, skipping a wrapper
`c1ba`/`c1f3`/`c23f`/`c280` that a normal D-pad press goes through and
which presumably commits the actual scroll position) -- but B/X were
never confirmed as real fast-travel buttons on bsnes, so whether that's
a bug or an unrelated quirk of those two buttons is still unknown.

**Contradiction resolved (autonomous follow-up, same session)**: the
"active dispatcher" premise was wrong, not the trace. `01:8b3b-8b42`
(part of the *same* function as the `01:8bd3`-`01:8c52` dispatcher
above, just earlier in it) branches on `$d7`: `$d7==0` falls through to
the buggy dispatcher already described; `$d7==1` does `JMP $8c55`, a
**second, near-identical dispatcher** whose own Y-check (`01:8c75`) has
**no** `$01f5==0` precondition -- so Y+direction is *not* blocked there.
`$d7` was previously documented as "always observed as 0" (see the
`$00d7` WRAM entry in `docs/ROM_MAP.md`), but that was apparently never
sampled during actual main-map fast-travel use.

Dispatcher 2's Y-path (reason code `6`, table entry `01:88ef+12` ->
`01:9f2d`) does substantial setup (clears `$01c1`/`$01f5`, sets several
flags to `$ffff`, sets `$01df=3` -- the same "screen-mode index" already
in the WRAM table) and ends with `JMP $9dcc`. That routine looks like an
**entry into a task-scheduler mechanism**: it writes into tables at
`$30c2,X`/`$ef20,X`/`$4420,X`, indexed by `$01df` doubled, then returns
immediately -- i.e. reason code 6 doesn't move the cursor synchronously,
it *schedules* a deferred task (mode 3) that presumably does the actual
scroll update on a later frame. This may be the same task-scheduler
mechanism the cursor-cadence investigation has been looking for (see
`docs/REVERSE_ENGINEERING_cursor_movement.md`'s bank-`0d` jump-table
lead) -- worth checking whether they're the same thing.

**Not yet confirmed**: what the scheduled mode-3 task actually does, or
why it doesn't appear to run in this recomp (the open bug). Found real
consumers for two of the three tables (`00:b66a: LDY $30c2,X`;
`01:f309: LDY $4420,X`), confirming they're genuinely read back
somewhere, not dead data -- but static disassembly around both readers
came out heavily misaligned (many unknown-opcode bytes), meaning the
true M/X flag state entering those functions isn't known and any further
decoding there would just be guessing. Stopped here rather than keep
pushing on an unreliable decode -- this needs either finding a clean
entry point to trace flags forward from, or live tracing (`SC_ADDR_TRACE`
on `00:b66a`/`01:f309` while triggering fast travel would settle it
directly).

## Open item: View screen's D-pad has no visible effect yet

`01:f0d3` (variant 6 above) is fixed, and live tracing confirms the fix is
real: pressing a direction now reaches a 4-iteration direction-dispatch
loop at `01:f0d3-f119`, which for each pressed direction calls a subroutine
at `01:f17d` that reads/adjusts/clamps a value in WRAM at `$7e21b4` (or the
companion byte at `$7e21b5` when a clamp limit is hit) and writes it back --
all confirmed firing live via `SC_ADDR_TRACE` on the write sites
(`01:f18c`, `01:f1ae`, `01:f1bb`). But a live before/after screenshot
comparison while repeatedly pressing a direction showed no visible change
beyond normal per-frame animation. So `$7e21b4`/`$7e21b5` is a genuine,
now-live piece of game state that isn't (yet) known to be read by anything
that renders -- either it feeds a rendering path we haven't traced, or the
renderer reads a different/cached copy of whatever position this
represents. Given the View screen's only other known problem (the tilted
map not rendering) turned out to be a missing-HDMA engine gap (see
`docs/INVESTIGATION_hdma.md`), the next step here is probably the same
kind of hunt: SC_GFX_TRACE or a live bsnes comparison to find what actually
reads `$7e21b4`/`$7e21b5` for rendering.

**Autonomous static-analysis update**: searched the ROM for absolute/long
references to `$21b4`/`$21b5`. Most hits in "high" banks (0c-0f) are false
positives -- those banks are compressed graphics/text data (see
`tools/extract_graphics.py`), not code, and the byte pattern searched for
coincidentally appears in the compressed bytes. Two genuine hits in
confirmed-executable banks:
- `00:c0fb`: `STA $7e21b5` writes `#$e0` (the same "at clamp limit"
  sentinel value found earlier at `01:f1bb`), then does the same to
  `$7e21b9`, `$7e21bd`, `$7e21c1`, ... `$7e21d5` -- a regular stride of 4
  bytes, 8 times. This looks like a **generic initialization pass over an
  array of position-tracking "slots"**, of which the View screen's
  `$21b4`/`$21b5` pair is only one -- i.e. this WRAM region is likely
  shared/reused by multiple UI elements, not View-screen-specific.
- `05:9c73`-`9c8a`: increments *both* `$7e21b1` and `$7e21b5` by 2 each,
  unconditionally, then `JMP $9e2b`. Not yet traced further -- this could
  be the actual renderer (or a different consumer entirely) advancing a
  position each call, but whether it's reached during the View screen or
  some other context isn't confirmed.
Next step (needs live testing): read-breakpoint `$7e21b4` in bsnes while
on the View screen to see exactly what reads it, rather than more static
guessing.
