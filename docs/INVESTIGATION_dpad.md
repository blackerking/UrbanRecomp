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

## Fast travel: root cause found (with live bsnes tracing), fix not yet attempted

**Status: broken, root cause identified and confirmed live in this
recomp specifically -- not yet fixed.** Everything below this point
superseded a long chain of earlier static-only theories (dispatcher
`$c5` reason codes, the `$d7` dispatcher-select state machine, a
speculated `01:9dcc` task-scheduler) that all turned out to be dead
ends or false trails once checked against real hardware. Keeping this
writeup linear rather than rewriting history, since the dead ends
themselves are informative about how not to chase this class of bug
(see "Method note" at the end).

**The real mechanism (confirmed via live bsnes instruction tracing,
same ROM)**: holding SNES Y or SNES A together with a direction causes
`01:afc6: inc $01bd` to fire every frame the combination is held --
this is the actual scroll-increment instruction, inside a routine at
`01:afbe` that's called unconditionally from the tail of `01:8d26` (the
same handler that draws the four arrow-cursor indicators). `01:afbe`
gates the increment on bit 0 of `$01c1`. On bsnes, holding Y or A sets
that bit correctly and the instruction fires every time; holding
neither, or D-pad alone, never fires it.

**Confirmed live in this recomp (via `SC_ADDR_TRACE`, after fixing two
tooling bugs that were making this untestable -- see "Tooling fixes"
below)**: `01:afc6` **never fires**, holding the equivalent buttons
(keyboard A/X per this recomp's keymap) for as long as needed. Tracing
further upstream (`01:8d40`, `01:b03c`, `01:afc0` -- the full
`$01c1` set/clear/read chain) found **none of those fire either**,
which pointed at something more fundamental than `$01c1` bookkeeping.

**Actual root cause, found by tracing the raw button state itself**:
`SC_ADDR_TRACE` on `01:c021` (right after `01:c01e: LDA $011b`, the
modifier-button check `$011b & $4080` that gates reason code 1) showed
**`$011b` reads as `$0000`, constantly, regardless of what's held.**
This is a 16-bit load, so it spans `$011b` (low byte) and `$011c` (high
byte) together.

Traced `$011b`/`$011c`'s actual writer: the shared edge-detector
(`00:928f-92cb`, entered via a JSL wrapper at `00:9278`/`00:927c`) does
correctly write `$011b,X` (`00:92c7: STA $011b,X`) with fresh
`$4218,X`-sourced data, in the *same* call that also writes `$c9,X` and
`$0123,X` two lines earlier -- both of which are confirmed working
(they're what the existing D-pad fixes rely on). So the writer is real
and clearly executes successfully for its other targets. The entry
point (`00:927c`) sets `Y=4, X=0` before falling into the loop --
processing all 4 SNES controller ports (`$4218`, `$421A`, `$421C`,
`$421E`, spaced by 2), and **includes a real hardware-timing wait**
(`00:9280: LDA $4212; AND #$01; BNE $9280`, busy-waiting on the
auto-joypad-read-in-progress flag) before reading the ports.

**Two live possibilities, not yet distinguished**: either (a) this
edge-detector call happens to run *after* the fast-travel check reads
`$011b` for the current frame (a call-ordering gap specific to this
recomp -- the `$c9`/`$0123` consumers just happen to run later in the
same frame, masking the same underlying issue), or (b) the `$4212`
busy-wait behaves differently under this recomp's auto-joypad-read
timing model than on real hardware, causing this call to read before
the hardware mirror is actually populated. Either way, this is very
likely the **same root-cause class** as the entire original D-pad bug
family (a WRAM mirror not being populated the way working code paths
assume) -- just at a read site (`01:c01e`/`01:c105`/`01:c12a`, all
direct `$011b` reads) that was never part of the earlier `$011a`-based
D-pad fixes, because those fixes only covered *direction* checks, not
this *modifier-button* check.

Also found: `00:9278` is dispatched through a jump table at `01:8fda`
(6 function pointers: `91c4, 9200, 923c, 9278, 932c, 92f0`), which looks
like a genuine per-frame task-scheduler -- possibly the same mechanism
`docs/REVERSE_ENGINEERING_cursor_movement.md`'s bank-`0d` lead was
looking for. The table's actual dispatch site (what indexes into it and
calls through it) wasn't found this session -- a raw-byte search for
references to `$8fda` hit only a false positive (coincidental bytes
inside unrelated `LDA`/`STA` long instructions in bank 03).

**Next step**: confirm which of the two possibilities above is real,
most directly via live-tracing `00:92c7` (`STA $011b,X`) alongside
`01:c01e` (`LDA $011b`) in the *same* run to see their relative
frame-timing and values, or by finding `01:8fda`'s actual dispatcher to
understand the task-scheduler's per-frame ordering.

### Method note: what actually worked vs. what didn't

Every static-only theory this session produced (the `$c5` dispatcher
chain, the `$d7` state machine, `01:9dcc` as a scheduler) was
individually clean, self-consistent, and wrong or unconfirmable --
each one only got resolved (either confirmed or discarded) once checked
against a live bsnes trace or a live `SC_ADDR_TRACE` run in this
recomp. Flag-tracked static disassembly is good at finding *candidate*
mechanisms and is a real prerequisite for knowing what to trace, but on
its own it kept producing plausible-looking dead ends here, matching
the same lesson `docs/INVESTIGATION_cursor_cadence.md` already recorded
for the cadence bug. The live bsnes instruction-level trace (not just
video/input comparison) was what actually cracked this -- worth
reaching for early next time rather than as a last resort.

### Tooling fixes made along the way (also useful beyond this bug)

- `SC_ADDR_TRACE`'s cadence side-watches (reads of `$011b`/`$011c`,
  writes of `$01eb`/`ec`/`ed`/`ee`/`$7c`) used to piggyback on *any*
  `SC_ADDR_TRACE` use at all, regardless of which addresses were
  actually being traced -- flooding stderr with hundreds of lines/frame
  and making the window unplayably slow whenever tracing something
  unrelated. Now gated behind their own `SC_CADENCE_WATCH` flag.
- The per-opcode PC-history write (feeding `SC_ADDR_TRACE`'s "last 128
  PCs" dump) used to run unconditionally from frame 0 the moment
  `SC_ADDR_TRACE` was set, regardless of an `@start` delay -- now gated
  on `s_addr_trace_start_frame` too, so a delayed start genuinely avoids
  all overhead until then.

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
