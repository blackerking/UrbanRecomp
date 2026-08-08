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
investigation. The map's "fast travel" modifier (B or X held while
moving, for a bigger/faster scroll jump) is now **also fixed** -- see
"Fast travel" below. One known remaining gap: the View screen's D-pad
now genuinely updates its underlying WRAM state (confirmed live) but
nothing visible changes on screen yet -- see "Open items" at the bottom.

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

## Fast travel: FIXED

**Status: fixed and confirmed end-to-end.** See "Resolution" below for
the final root cause and fix -- it turned out to be neither the
mechanism nor the modifier button described in the investigation history
immediately below, both of which were reasonable conclusions from live
tracing at the time but didn't survive a later, more controlled test.
Keeping the full history rather than deleting it, since the dead ends
are informative about how not to chase this class of bug (see "Method
note" further down) and about how even *live* tracing can mislead when
the input source itself (a human hand on a keyboard) isn't fully
controlled.

### Investigation history (superseded, kept for the method-note value)

**Status at the time: broken, root cause identified and confirmed live
in this recomp specifically -- not yet fixed.** Everything below this point
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

**Resolved (confirmed live, same session)**: it's option (a), a
call-ordering/scheduling mismatch, not a `$4212` timing bug or a
never-populated mirror. Two pieces of same-session evidence:

1. `SC_ADDR_TRACE` on `00:92c7` (the writer) showed it firing correctly
   every frame, 4 times per frame (one per controller port) -- but every
   single logged value across a long session was `0x0000`.
2. `SC_ADDR_TRACE` on `01:c01e` (the fast-travel modifier read) showed
   it firing only *periodically* -- roughly every 4 frames (observed at
   consistent 4-frame spacing: `f=1463, 1467, 1471, 1475, 1479, ...`),
   meaning whatever gates this dispatcher only gives it a turn on some
   frames, not every frame. Every logged value at those specific
   instants was also `0x0000`.
3. **But** a new on-demand WRAM-dump hotkey (`F4`, see below) taken
   *in between* those instants, at frame 1421, captured `$011b` (16-bit,
   spanning `$011b`+`$011c`) as `0x0082` -- genuinely nonzero, and
   `0x0082 & 0x4080 = 0x0080`, which would satisfy the modifier check if
   read at that exact moment.

So `$011b` **does** hold correct data at times -- it's just that the
specific once-every-~4-frames instant `01:c01e` happens to run doesn't
line up with when that data is actually present; something clears or
overwrites it before this particular dispatcher's turn comes around.
This is very likely the **same root-cause class** as the entire
original D-pad bug family (a WRAM mirror not surviving to the moment a
particular consumer expects it) -- just manifesting as a scheduling
race here rather than a permanently-wrong address.

**Retracted, autonomous follow-up**: an earlier pass in this same
session speculated that `00:9278` (the edge-detector) is dispatched
through a jump table at `01:8fda` (6 "function pointers":
`91c4, 9200, 923c, 9278, 932c, 92f0`), and framed that as the likely
task-scheduler responsible for the periodic gating above. Checked more
carefully afterward and this doesn't hold up: three of those six
addresses (`91c4`, `9200`, `923c`) form a genuine, self-consistent
bytecode-VM opcode-handler pattern (reads a script buffer at `$0000,Y`
via a PC at `$0009`, writes to `$7e8000,X` -- plausibly the title-screen
logo animation, given it fires around frame 50) -- but the other two
(`932c`, `92f0`) turned out to just be the middle of two *already*
separately-disassembled, unrelated functions (`92f0` is literally
mid-body of the `00:92cc` routine documented elsewhere in this file).
That's not a coherent task list, and an exhaustive search for any
reference to `$8fda` (immediate load, indexed load, indexed-indirect
`JSR`) found none at all. **The `01:8fda` table is very likely
coincidental bytes, not a real dispatcher** -- retracting the claim
rather than let it stand uncorrected. What actually gates `01:c01e`'s
periodic execution, and what clears `$011b` in between, is still
unknown.

**Next step**: this needs live tracing, not more static guessing --
static analysis has now produced multiple dead ends on this specific
question (the `$c5`/`$d7`/`$9dcc` chain earlier, and this `$8fda` table
just now). Most direct: a live trace on every write site touching
`$011b` across several consecutive frames (not just `92c7`) while
holding the fast-travel modifier, to see what runs *between* the
correct-data moment (like frame 1421) and the next `01:c01e` check that
reads it as zero.

### Resolution

The breakthrough was switching from live keyboard/bsnes testing to
**fully deterministic** testing: numbered save-state slots (new
`Shift+1..0`/`1..0` hotkeys, `--load-state`) let a specific scenario (on
the map screen) be captured once, then `--input <frame>:<duration>:
<hexmask>` reproduces an exact button hold with zero live-input timing
jitter -- no human hand, no SDL polling gaps, no keyboard scan-rate
limits.

**First surprise**: with `$011b`/`$011c` genuinely held (via `--input`,
not a physical key) and a dedicated write+read watch on both addresses
(`SC_CADENCE_WATCH`, extended this session -- see "Tooling fixes"),
`01:c01e` read the correct, nonzero value on *every single hit* across
hundreds of frames. The "scheduling race" theory above -- that something
clears `$011b` between the edge-detector's write and this periodic read
-- does not reproduce under deterministic input. It's retracted; the
original "always reads `$0000`" observation was very likely a live
keyboard-timing artifact (a human can't hold two keys down with
frame-perfect precision across a ~4-frame window), not a real bug.

**Second surprise**: holding SNES Y+direction (via `--input`, mask
`kPad_Y|kPad_Up`) never sets `$c5` to `1` -- it's `0` (no-op) or `2`
(the Y-alone "toggle advisor" reason, unrelated to movement) instead.
Re-deriving the `$011b`/`$011c` bit layout carefully against this
project's *own* already-verified D-pad-fix comments (`src/main.c` lines
~854-890, cross-checked across 9 real screens via interactive testing)
settled it: `$011b` (low byte) = `B/Y/Select/Start/Up/Down/Left/Right`,
`$011c` (high byte) = `A/X/L/R` + a hardware-dead low nibble. The
`$011b AND #$4080` check that gates reason code `1` (at `01:8bd6`) tests
bit 7 of the low byte (`$011b`, = **B**) and bit 6 of the high byte
(`$011c`, = **X**) -- **B or X, not Y or A**. The "holding SNES Y or A
fires `01:afc6`" claim earlier in this doc was a mislabeling from an
earlier live bsnes session (this project has a long history of
recomp-keymap-vs-bsnes-keymap and QWERTY/QWERTZ mixups -- see the
"Errors and fixes" pattern throughout this session's own transcript);
whatever that session actually held was very likely B or X, not Y/A.

**Root cause, finally**: holding B/X+direction *does* reach `01:8d26`
(reason 1, confirmed: `$c5`=1, 200/200 hits under a deterministic
300-frame B+Up hold) -- but `01:8d26`'s own direction-nibble read, at
`01:8d36`, has **the exact same "wrong nibble" bug** as the 8 sites this
project's `main.c` D-pad fix already patches at load time, just in a
different instruction shape that the original byte-pattern scan
(`AD xx xx 29 00 0F`) didn't match:

```
01:8d36  LDA $011b     ; 16-bit: low=$011b, high=$011c
01:8d39  SEP #$20      ; switch to 8-bit A
01:8d3b  XBA           ; swap A's bytes -- A's low byte is now $011c
01:8d3c  AND #$0f      ; tests $011c's low nibble -- hardware-dead, always 0
01:8d3e  BEQ $8d8c     ; always taken
01:8d40  STA $01c1     ; (unreached) would store the real direction nibble
```

The `XBA` after the 16-bit `LDA $011b` swaps `$011c` (the dead nibble)
into the position the 8-bit `AND #$0f` tests, instead of `$011b` (where
the real D-pad bits live). Since the `AND` is always zero, the `BEQ`
always takes -- so `$01c1` never gets written and none of the 4
per-direction handlers (`b2f9`/`b1f6`/`b166`/`b030`) ever run, for *any*
held direction. `01:afbe` (the actual `$01bd`/`$01bf` scroll-increment
bit-ladder, previously misdescribed as gating on "`$01c1` bit 0" when
it's really a 4-bit ladder over `$01c1`'s low nibble) is downstream of
this and never fires either -- not because of anything wrong with
`$01c1`'s own read/gate logic, but because it never gets written in the
first place.

**Fix** (`src/main.c`, same D-pad-fix block, "Ninth site" comment): same
technique as the other 8 sites -- repoint the `LDA $011b` at `01:8d36`
to `LDA $011a`, so `$011a` (irrelevant, discarded by the `AND #$0f`
after XBA) lands as the load's low byte and `$011b` lands as the high
byte, which `XBA` then swaps into A's low byte where the existing,
unmodified `AND #$0f`/`STA $01c1`/dispatch ladder already expects real
direction bits.

**Confirmed end-to-end** via the same deterministic method: holding
B+Up now reaches `b2f9` (227/300 hits) and `01:afbe` (200/200 hits,
trace budget cap), and `01:afd8` (`DEC $01bf`, Up's specific ladder
rung) fires repeatedly; a WRAM dump before vs. ~13 hits later shows
`$01bf`/`$01c0` (scroll-Y, 16-bit) going from `0x0006` to `0xfffa` --
genuinely decrementing. B+Right similarly reaches `01:afc6` (90/300
hits). Zero regression on the full 10800-frame `--qualify` baseline
(identical `logic_changes`/`audio_samples`/`audio_active_frames`/
`video_changes` to the numbers already documented in `README.md`).

Whether real SNES Y/A are *also* supposed to trigger some form of fast
travel through a different, still-unfound path is now a separate, lower-
priority open question -- the `$011b AND #$4080` gate reads like a
deliberate "B or X held" check (matching the same "deliberate
non-direction gate" pattern already noted at `01:8958`/`01:8c68`/
`02:a4ec` elsewhere in this file), not a bug, so there's no specific
reason to expect a parallel Y/A path exists. The `01:8c55`
second-dispatcher lead (gated on `$d7==1`) remains unconfirmed: a
targeted scan of all 9 `STA $d7` sites in the ROM found none of them
execute even once across 300 deterministic frames of held Y+direction.

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
- Added an on-demand WRAM-dump hotkey (**F4** in windowed mode, see the
  README controls table) -- writes a full WRAM snapshot to
  `wram_snapshot.bin` in the working directory at the exact moment
  pressed. This is what actually cracked the scheduling-race finding
  above: a live snapshot at a specific instant, cross-referenced against
  `SC_ADDR_TRACE` hits in the *same* session, showed `$011b` holding
  correct data at a moment the periodic trace never landed on.
- `SC_CADENCE_WATCH`'s write-side watch now also covers `$011b`/`$011c`
  themselves (previously only the mouse-cursor bytes), paired with the
  existing read-side watch on the same two addresses -- one run now
  shows every read *and* write to both bytes, in order, across
  consecutive frames. This is the concrete tool the "next step" above
  calls for (a live trace of every write site touching `$011b`, not just
  the known `92c7` one), to find what clears it between the
  edge-detector's write and `01:c01e`'s once-every-~4-frames read.
- Added numbered save-state slots (**Shift+1..Shift+0** to save,
  **1..0** to load, see the README controls table) plus a headless
  `--load-state <path>` flag. Captures the full emulator snapshot (WRAM +
  CPU registers + every device model, via the shared runner's own
  `snes_saveload`/`interp816_saveload`) so a specific scenario -- on the
  map screen, cursor visible, nothing else held -- can be set up once by
  hand and then reloaded instantly and deterministically for repeated
  fast-travel testing, instead of re-navigating menus or guessing
  `--input` timing on every run.

## View screen's D-pad: FIXED -- 10th site in the dead-nibble family (`01:8c6b`)

**This is the fix that actually made the View screen respond to the
D-pad.** It is separate from, and supersedes in practical importance, the
sprite-109 cursor dead-end documented immediately below (which remains
accurate: that *cursor* is still unrenderable -- but the cursor was never
what "moving the View screen" meant).

User report that cracked it: on this recomp the View screen scrolled only
while holding B or X, whereas *the original cartridge scrolls on the
D-pad alone*. That points at `01:8c68`, which computes the reason code
stored to `$c5` -- the value that selects the `01:8d26` dispatcher, whose
tail `01:afe0` writes the real PPU scroll `$0137`/`$0139`:

```
01:8c68  AD 1B 01   LDA $011b      ; 16-bit: low = $011b, high = $011c
01:8c6b  29 80 4F   AND #$4f80
01:8c6e  F0 05      BEQ $8c75
01:8c70  A9 01 00   LDA #$0001     ; reason code 1
```

Decomposing the mask:

| term | tests | real? |
|---|---|---|
| `$0080` | `$011b` bit 7 = B | yes |
| `$4000` | `$011c` bit 6 = X | yes |
| `$0f00` | `$011c` bits 0-3 | **no -- hardware-dead nibble** |

That `$0f00` term is this ROM's D-pad-bug signature. The author plainly
meant "B **or** X **or any direction**", but the direction bits live in
`$011b` bits 0-3 -- the *low* byte -- so the term is structurally dead and
only B/X ever set the reason code. These notes previously dismissed this
site as "a deliberate non-direction gate ... left alone"; the `$0f00`
term is what makes that reading untenable.

**Fix:** widen the mask's low half, `AND #$4f80` -> `AND #$4f8f`, so
`$011b` bits 0-3 also satisfy it. One byte (file offset `0x8c6c`).
Deliberately *not* the usual `$011b`->`$011a` shift used elsewhere in this
family: that would move B's `$0080` test onto `$011a` and X's onto `$011b`
bit 6 (Y), breaking both real button checks.

Verified with `--load-state` + `--input` against the View screen save
state (reason code `$c5` now becomes 1 for a bare direction, and the real
scroll registers move):

| input | `$0137` | `$0139` | `$c5` |
|---|---|---|---|
| none  | `90` | `20` | `00` |
| up    | `d8` | `20` | `01` |
| down  | `58` | `20` | `01` |
| left  | `90` | `c8` | `01` |
| right | `90` | `d8` | `01` |

Screenshots confirm the tilted map visibly scrolls in all four
directions with the perspective and desk framing intact. Scope is
correct: `01:8c68` is only reached via `01:8c55`, the `$d7`==1
dispatcher, so the normal map screen (`$d7`==0, which routes through
`01:c0dd`) is untouched -- re-verified by injecting directions into the
normal-map save state and confirming `$c5`, scroll and cursor all behave
exactly as before. `--qualify 400` still passes.

## View screen's *cursor sprite*: unrenderable by construction (separate dead end)

**Status: closed.** The position data is live and (mostly) correct; there
is simply no sprite for it to draw. Established by loading the View
screen save state and injecting each direction deterministically
(`--load-state` + `--input`), then reading sprite 109's *entire* OAM
shadow entry rather than only the two position bytes:

| input | X (`$21b4`) | Y (`$21b5`) | tile (`$21b6`) | attr (`$21b7`) | X-high |
|---|---|---|---|---|---|
| none  | `80` | `e0` | `00` | `00` | 1 |
| left  | `d8` | `e0` | `00` | `00` | 1 |
| right | `28` | `e0` | `00` | `00` | 1 |
| down  | `80` | `d0` | `00` | `00` | 1 |

These are end-of-frame values -- precisely what DMA channel 0 copies to
real OAM -- so they are what the PPU actually sees. Two independent,
individually sufficient reasons the cursor can never appear:

1. **`tile=$00`, `attr=$00`**: no cursor graphic is ever assigned to
   sprite 109. It draws tile 0 with palette 0.
2. **X-high = 1** (bit 2 of the high-OAM byte `$221b`, set every frame by
   `00:c0fb`'s icon-rebuild loop via `AND #$03 / ORA #$54`): the sprite's
   real X is `256 + $21b4` = 384-472, entirely off a 256-pixel screen.

Sprites 124-127, previously suspected of being the "real" icon, are all
`X=80 Y=e0 tile=00 attr=00` -- blank and parked. Freezing their `$0b03`
gate to 1 (so `00:c189` stops re-hiding them) changes nothing on screen,
confirming they are not the cursor either.

**Conclusion:** the View screen's cursor is unfinished/vestigial in this
ROM revision. `01:f189`/`f190`/`f19a`/`f1bf` faithfully compute and clamp
a position into sprite 109's OAM slot, but nothing ever gives that slot a
tile or brings it on-screen. This is not a recomp bug, and no host-side
fix is warranted -- making it visible would mean *authoring* a cursor
(assigning a tile, clearing the X-high bit), i.e. a new feature, not a
correction. The `$7e21b5` stomp fix below remains correct and worth
keeping (it makes the Up/Down value persist as designed); it was simply
never sufficient, because the sprite was unrenderable regardless.

**Secondary finding, left open:** Up is asymmetric with Down. Holding
Down moves `$21b5` `e0`->`d0`, but holding Up leaves it at `e0`. Left and
Right both work (`80`->`d8`/`28`). So the Up handler
(`01:f19a`/`f1bf` region) has a real bug of its own -- currently moot,
since nothing renders the result, but worth fixing if the cursor is ever
given graphics.

Everything from here down is the original investigation, kept for the
reasoning trail (including two leads that looked right and were
disproven).

---

**Earlier status: partially resolved.** The `$7e21b5` stomping bug described below
is real, confirmed, and fixed -- `$7e21b5` now updates and persists
exactly like `$7e21b4` always did. But live user testing after the fix
still shows no clean visible movement, just flickering (matching the
original report for this screen from before any of this session's work:
"glitches a little bit when arrow pad is used, but it doesn't move") --
for *both* axes, including `$7e21b4`, which was never broken at the WRAM
level. So there's still a separate, unfound problem in the actual
rendering path: something needs to read `$7e21b4`/`$7e21b5` and turn it
into a moved sprite/scroll/window position, and nothing in the entire ROM
does that as a plain memory read (confirmed via the live
`SC_VIEW_WATCH` watch, which catches every addressing mode). Leading
theory, not yet confirmed: the connection is via DMA (which bypasses
CPU-level memory-read hooks entirely, unlike a `LDA`), tying into the
same OAM-icon-rebuild DMA machinery `00:c0fb`'s loop sets up -- worth
checking with `SC_GFX_TRACE` for DMA channel setups that reference this
WRAM region as a source address, or a live bsnes read-breakpoint on
`$7e21b4` for a real hardware ground truth. The write-side fix below is
still real and worth keeping either way; it's just not sufficient on its
own.

**Follow-up: chased the DMA theory, hit a genuine wall.** `SC_GFX_TRACE`
while holding a direction on the View screen found DMA channel 0
transferring 544 bytes from `$7e2000` to `$2104` (the OAM data port)
every single frame -- the shadow OAM buffer for all 128 sprites.
`$7e21b4`/`$7e21b5` sit exactly at offset `0x1b4` into it: **sprite
#109's X and Y bytes**. This looked like the answer -- except a
before/after WRAM dump of the *other* sprites sharing that default
position (`$7d`/`$e0`) showed sprites 124-127 (a real, 4-tile 16x16 icon
with actual graphics -- tiles `00/02/06/04`, `attr=$38`, unlike sprite
109's blank `tile=$00`/`attr=$00`) staying **completely frozen** while
sprite 109 correctly tracked the D-pad. They only shared a coincidental
default position, not an actual connection -- confirmed by searching the
View screen's own code (`01:f000`-`f250`) for any reference to what
gates sprites 124-127 (a flag at `$0b03`, found via the same DMA/OAM
tracing: a routine at `00:c189` resets sprites 124-127's Y to `$e0`
every ~4 frames unless `$0b03` is nonzero) -- zero references found.
Sprite 109 is genuinely unused/idle OAM space that the View screen's
Left/Right and Up/Down logic happens to borrow as scratch storage; it
has no bearing on what's actually drawn.

Exhaustively reconfirmed after this dead end: `$7e21b4`/`$7e21b5` have
exactly 4 consumers in the *entire* ROM -- `01:f189`/`f190` and
`01:f19a`/`f1bf` themselves (plus the now-fixed `00:c0fb` stomp).
Nothing else reads or writes them, anywhere, via any addressing mode.
Static analysis has now produced and personally disproven three
plausible-looking leads on this specific question (the stale
`$7e21b4` static-scan read, the sprite-109 OAM coincidence, the `$0b03`
jump-table guess) -- the same "confirmed live, not by guessing" lesson
this document's Method Note already drew from the fast-travel
investigation applies again here. **Concrete next step, matching what
actually worked for fast travel**: a live bsnes capture of the View
screen with a direction held, watching whether *anything* visibly moves
there on real hardware either. If nothing does, this is very likely
dead/unfinished code in this ROM revision, not a recomp bug, and the
`$7e21b5` stomp fix (still real, still worth keeping) was simply
unrelated to the visible symptom. If something *does* move on real
hardware, a read-breakpoint on whatever WRAM byte changes at that moment
is the fastest way to find the real position variable -- almost
certainly not `$7e21b4`/`$7e21b5`.

`01:f0d3` (variant 6 above) was already fixed and live tracing confirmed
the write side was real: pressing a direction reaches a 4-iteration
direction-dispatch loop at `01:f0d3-f119`, calling a subroutine at
`01:f17d` that reads/adjusts/clamps a value in WRAM at `$7e21b4` (Left/
Right, via `01:f189`/`f190`) or `$7e21b5` (Up/Down, via `01:f19a`/`f1bf`)
and writes it back. But nothing visibly changed on screen.

**Root cause, found via deterministic `--load-state`/`--input` testing
plus a new live memory watch (`SC_VIEW_WATCH=1` in `src/main.c`, catches
every addressing mode including dynamic/indirect ones a static opcode
scan can miss)**: `$7e21b4` (Left/Right) was never actually broken --
confirmed live, it cleanly decrements frame-over-frame while held (e.g.
`7d->7a->77->74->...`). `$7e21b5` (Up/Down) was the real problem: a
universal, always-on per-frame routine at `00:c0fb`
(`SEP #$20; LDA #$e0; STA $7e21b5`, part of a loop at `00:8aa8` that
rebuilds a whole row of UI icon sprites into OAM via DMA every frame, on
*every* screen -- confirmed also firing on the classic map, not View-
specific) unconditionally resets `$7e21b5` to a fixed `$e0` every single
frame, stomping whatever `01:f1bf` just computed before anything could
read the update. Confirmed live holding Down: `01:f1bf` computes a real
new value (e.g. `$dc`), but `01:f1a7` (the *only* reader of `$7e21b5`
anywhere in the ROM -- confirmed via the same live watch, on every
screen) only ever observed the reset value `$e0`, never the update. A
static byte-pattern scan for `LDA $7e21b4`/`$7e21b5` had turned up only
one match total, and it was a red herring (an unrelated read-modify-write
increment in bank 5) -- this needed the live watch to find the real
consumer and the real saboteur, neither of which a static scan alone
would have surfaced.

**Fix**: NOP out just the one `STA $7e21b5` instruction (4 bytes,
`EA EA EA EA`) inside `00:c0fb`, leaving its six sibling table-slot
writes (`$7e21b9`/`bd`/`c1`/`d5`/`d9`/`dd`, part of the same per-frame
icon rebuild) completely untouched -- as surgical as a byte patch gets.
Safe because `$7e21b5` has exactly one consumer in the whole ROM (the
View screen's own Up/Down check, confirmed via the live watch across
multiple screens); nothing else reads it, so skipping this one reset
can't leave stale data visible anywhere else. Confirmed live: `$7e21b5`
now cleanly decrements the same way `$7e21b4` already did. Zero
regression on the full `--qualify` baseline.

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
