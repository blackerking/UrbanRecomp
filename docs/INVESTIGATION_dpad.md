# D-pad investigation (resolved)

Status: **fixed**. The D-pad now works on every screen tested: map
scrolling, the build cursor, the toolbar, in-game menus, the Information
panel, the mode-select (start) menu, Scenario Select, Save, Tax, and the
Load/Save/Exit menu. Confirmed by interactive testing on every screen, and
independently cross-checked against real hardware-accurate emulation
(bsnes) partway through the investigation. One known remaining gap: the
map's "fast travel" modifier (X or Y held while moving, for a bigger/faster
scroll jump) is not yet fixed -- see "Open item" at the bottom.

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
the byte that actually holds the direction bits. Four distinct shapes of
the same underlying bug were found, each needing a slightly different
patch:

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
   Tax-screen modal, and several of the 20 handlers in the mode-select/
   list-menu dispatch table at `03:d255`). Since these are single-byte
   loads (not 16-bit), the fix is simpler: just repoint the load at `$c9`
   directly. 2 sites (`02:a50c`, `02:ab1f` -- later found to be dead code,
   see below) + 1 site (`03:d33e`, mode-select) + 3 more sites across modes
   5 and 11 (Scenario Select / Save).
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
   ladder involved.** The Load/Save/Exit top-level menu (`00:d1b8`) tests
   bits 8-9 of the combined word directly (no ASL/BCC ladder to desync),
   so the same address-shift fix as variant 1 applies (`$c9`(dp)→`$c8`(dp))
   with no extra care needed. One site fixes a symmetric Left/Right pair
   (`00:d1c4` decrements, `00:d1db` increments a shared selection-index
   byte at `$0421`, both sharing this one gate). Two neighboring checks at
   `00:d1aa` (`AND #$8000`, tests `$ca` bit 7 = A) and `00:d1b1`
   (`AND #$0040`, tests `$c9`'s own bit 6 = Y) already read real, valid
   bits and were left alone.

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

## Open item: fast travel

Holding X or Y while moving on the map is supposed to trigger a faster/
bigger scroll jump (per interactive testing, confirmed still not working
after the D-pad fix above). `JSL $0098a0` was suspected as the relevant
call (originally spotted gated behind a Y|A check in the mode-select
dispatch family), but it turned out to be an extremely generic utility
with 90+ call sites throughout the ROM, making it impractical to trace
statically. A live RAM-diff (map, no modifier vs. X+direction held) turned
up `$1efd`/`$1efe` changing, but that traced back to the direct-page base
used by the background city-statistics scanner (walking `$7f0200,X` tile
data for the crime/pollution overlay) -- a false positive that changes
every frame regardless of input, since it's a continuously-running
background scan. Not yet resolved; a cleaner live-session approach (e.g.
the `F1`-reset bitmap-diff technique that worked for Tax, scoped tightly
around holding the modifier) is the natural next step.
