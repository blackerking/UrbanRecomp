# SimCity (SNES, US) ROM map

Consolidated reference for everything mapped out during this project's
investigations. This is a living document -- addresses/meanings here
reflect the current best understanding, not certainty; confidence is
noted inline where it matters. For the full investigative reasoning
behind any entry, follow the linked doc rather than trusting this file
alone if something looks off.

ROM: US release, 512KB (`0x80000` bytes), LoROM mapping. File offset for
any `bank:addr` with `addr >= 0x8000` is `bank*0x8000 + (addr-0x8000)`
(no copier header on this dump).

## Bank layout (confidence: high for 00-05, medium for 09-0f)

| Bank(s) | Contents |
|---|---|
| `00` | Core engine: boot, NMI/joypad handling, shared edge-detector (`00:928f-92cb`), LC_LZ5 decompressor (`00:90dd`), various shared utilities |
| `01` | Shared UI/cursor dispatch code: map cursor movement, mode-select ladder, direction-priority scanners, the `01:8b4e`+ per-frame dispatcher chain |
| `02` | Modal screens: Tax, Save, and similar bank-2-resident popups |
| `03` | Mode-select dispatch table (`03:d255`, 20 entries) and its handlers: Map Select, city-name-entry keyboard, Select-game-level, Scenario Select |
| `05` | Misc shared routines; at least one position-array stepper (`05:9c73`) not yet fully traced |
| `09`-`0f` | Compressed graphics/text data (font tileset, dialog text, scenario tileset/text -- see "Compressed data regions" below) and possibly a task-scheduler jump table (`0d:~e07a`, unconfirmed) |

## WRAM variable reference

Addresses are absolute WRAM offsets (`$7E:xxxx` unless noted `(dp)` for
direct-page, which resolves the same way when D=0, the common case in
this ROM).

| Address | Meaning | Confidence |
|---|---|---|
| `$00c9`/`$c9` (dp) | Edge-detect mirror, low byte -- real direction+face-button bits live in the **low nibble** here (see `$011b`) | High, extensively verified |
| `$00ca`/`$ca` (dp) | Edge-detect mirror, high byte -- **low nibble is hardware-guaranteed zero** (unconnected controller pins); real A/X/L/R bits are in the *high* nibble | High |
| `$00d7`/`$d7` (dp) | Bail-out flag checked at the top of several dispatch functions (`01:c0dd`, `01:948b`) -- confirmed always `0` in every live sample so far, role unconfirmed | Medium |
| `$0059`-`$005d` (`$59`/`$5b`/`$5d`) | Rolling checksum/hash accumulator pair, folds scenario parameters (`$0b27`-`$0b29`) via `00:824b`/`824f`; part of procedural map/seed generation (10-iteration loop at `03:d862`) | High (byte-level, live traced) |
| `$0079`/`$79` (dp) | Scratch: per-axis step delta in several contexts (cursor movement, decompressor table pointer) -- meaning is call-site-dependent | Medium |
| `$007a`-`$007e` | Scratch block used by a table-driven update loop at `00:94fd`-`95d8` (indexed via `$0b4d`) | Low, not traced in detail |
| `$007c`/`$7c` (dp) | **Reused scratch, not a single-purpose variable** -- among other uses, `00:cdec` treats it as a tight busy-loop delay counter (32 decrements within one frame); a red herring for the cadence investigation, see `docs/REVERSE_ENGINEERING_cursor_movement.md` | High (the "don't trust this" lesson is confirmed) |
| `$00c5`/`$c5` (dp) | "Reason code" written by the main-map cursor dispatcher when B/X (`1`) or Y (`2`) is held, right before an early `RTS` (`01:8c52`) -- consumer not yet found | Medium |
| `$011b` | `$4218` mirror (held-state). **Real D-pad + B/Y/Select/Start bits live in the low/high nibbles here** -- bit layout: bit0=Right,1=Left,2=Down,3=Up,4=Start,5=Select,6=Y,7=B | High |
| `$011c` | `$4219` mirror (held-state). Bits 4-7 = R/L/X/A (real); **bits 0-3 are hardware-guaranteed zero** | High |
| `$011a` | One byte before `$011b` -- the D-pad fix family's standard "shift the 16-bit load back one byte" target, so a load spanning `$011a`/`$011b` puts real direction bits where a buggy ladder expected zeroed `$011c` bits | High |
| `$0123`/`$0124` | Absolute (non-direct-page) mirror pair, same relationship as `$011b`/`$011c` | High |
| `$01bd`/`$01be` | Map scroll-X / scroll-Y (fast-travel investigation lead, not confirmed) | Low |
| `$01c1` | Referenced in early SC_DEBUG tooling; role not documented | Low |
| `$01d7` | Checked (`!= 0`) partway through `01:c0dd`'s gate chain; always `0` in live samples so far | Medium |
| `$01df` | "Screen-mode index" -- written `3` when the cursor dispatcher's interrupt path fires | Medium |
| `$01eb`/`$01ec` | X-axis position/cursor pair (Comprehensive screen ladder target, also read by the fast-travel-adjacent scroll-bounds check) | High |
| `$01ed`/`$01ee` | Y-axis (or primary) cursor/scroll position -- the byte the whole cursor-cadence investigation centers on; stepped by `01:c214` (increment) / `01:c1ca` (decrement), both landing via `01:c221`/`01:c1d5` | High |
| `$01f3` | Checked in `01:c0dd`'s gate chain; a decrement-and-RTS bail path exists if a sibling check (`$0201`) is zero | Medium |
| `$01f5` | Read by the direction-priority scanners (`01:c4c7`/`c4e5`) via a 4-step `LSR`/`BCC` ladder; always `0` in every live sample so far (branch targets never observed live) | Medium |
| `$01f9` | Direction/axis index feeding `CheckAxisScrollBounds` (`01:c434`)'s table lookup at `$0180c0,X` | Medium |
| `$01fb` | Referenced in early D-pad debug tooling (mode dispatch related) | Low |
| `$01ff` | Gates a jump to `01:c195` (bypassing the ladder at `01:c132`) when nonzero; the actual value never observed nonzero live | Medium |
| `$0201` | Major branch-point value: `0` on real hardware immediately after boot (routes through `01:c2e6`/`c2f3`, a much simpler direction check); `0xff` during live gameplay in every session sampled (routes through `01:c0fd`/`c105`/`c132`, the complex ASL-ladder path this whole session's cadence work has focused on). **Two genuinely different code paths depending on this value** -- see `docs/REVERSE_ENGINEERING_cursor_movement.md` "UPDATE 3" | High |
| `$0203` | Set from `$011b & $8000` (A-button held) inside `01:c0dd`; also used as a loop index (`LDX $0203`) in the `01:c162` ladder dispatcher | Medium |
| `$0253` | Zeroed at the start of `01:c616` | Low |
| `$0255`/`$0257` | X/Y-derived table indices inside the `00:8eb7`-area sprite/table update loop | Low |
| `$025b` | Bitmask accumulator inside the same loop (`00:8edf` onward, shift-and-test pattern) | Low |
| `$025d`/`$025f` | Copies of `$01eb`/`$01ed` staged for the `COP #$00` dispatch at `01:c63e` | Medium |
| `$0261` | Copy of `$01f9`, also staged for the same `COP` dispatch, and read again at `00:8eab` as a jump-table selector (`ASL A`; index into `$00a164,Y`) | Medium |
| `$0383`/`$0387`/`$0395` | Mode-flag bytes checked early in the cursor dispatcher (`01:8ba2`+); any nonzero value diverts to the shared interrupt tail (`01:8c52`) with a different reason code each | Medium |
| `$0389` | Checked (`!= 0`) in `01:948b`'s own `$d7`-gated bail chain | Low |
| `$03fe` | Checked early in `01:8b4f`'s dispatcher, gates whether `$0201`/fast-travel-adjacent `JSL $0098a0` logic runs at all | Low |
| `$0421` | Shared selection-index byte for the Load/Save/Exit menu and Select-game-level screen (both patched via the same D-pad fix family) | High |
| `$0b27`-`$0b29` | Scenario generation parameters, folded into the `$59`/`$5b`/`$5d` checksum loop | Medium |
| `$0b4b`/`$0b4d`/`$0b4f` | Control values for the `00:94d5`-area table-driven update loop | Low |
| `$0bcb` | Written `0x0a` alongside `$01df=3` on the cursor dispatcher's interrupt path | Medium |
| `$0c0f` | The **other**, already-D-pad-fixed cursor-mover's own gate byte (`01:ae2e`'s routine) -- confirmed *not* the same mechanism driving `$01ed`'s cadence (stayed `0` throughout live cadence sampling) | High |
| `$0dc3` | Checked alongside the non-direction-button test at the very top of `01:8b4f` | Low |
| `$21b1` | Incremented by 2 alongside `$21b5` at `05:9c73`, unconfirmed relation to the View screen | Low |
| `$21b4`/`$21b5` | View-screen cursor position pair, stepped by `01:f18c`/`f1ae`/`f1bb`; confirmed part of a generic 8-slot array (stride 4, initialized at `00:c0fb`) shared with other UI elements, not View-screen-exclusive | High (write side) / Low (what reads it for rendering) |

## Named routines

See `docs/REVERSE_ENGINEERING_cursor_movement.md` for full pseudo-C and
confidence notes on each -- summary table only below.

| Address | Name | Purpose |
|---|---|---|
| `00:928f-92cb` | *(shared edge-detector)* | XORs `$4218`/`$4219` against the previous frame; writes held/edge state to `$0123,X`/`$c9,X`/`$ca,X` |
| `00:90dd` | *(LC_LZ5 decompressor)* | Nintendo/community-named "LC_LZ5" compression; input bank/offset via WRAM `$09`/`$0b`, output to `$7E:0000+X`. See `tools/extract_graphics.py` for a verified-working reimplementation |
| `01:8b4f` | `CursorMoveDispatch_Frame` | Per-frame entry; checks non-direction buttons and a couple of state flags, bails to a reset path or falls into the mode-flag checker |
| `01:8b9f`/`8ba2` | `CursorMoveDispatch_CheckModeFlags` | Checks `$0395`/`$0383`/`$0387`, then B/X/Y held (writes `$c5` reason code + `RTS` if so), then falls into the direction ladder |
| `01:c132`/`c135` | *(direction ladder entry, already D-pad-patched)* | `LDA $011a; AND #$0f00` -- the fixed 16-bit direction test |
| `01:c155`/`c156` | *(the exact instruction gating the step, cadence investigation)* | 6th `ASL` of the direction word; the `BCC` here is what ultimately allows/blocks reaching `StepCursorAxis_*` |
| `01:c1ca` | `StepCursorAxis_Decrement` | `$01ed -= 2`, clamped at `0x18`, lands via `01:c1d5` |
| `01:c214` | `StepCursorAxis_Increment` | `$01ed += 2`, clamped at `0xd0`/`0xd1`, lands via `01:c221` |
| `01:c433`/`434` | `CheckAxisScrollBounds` | Per-direction delta lookup (`$0180c0,X`) and bounds pre-check against `$01eb`/`$01ed` |
| `01:c4c7`/`c4e5` | `DirectionPriorityScan_A`/`_B` | 4-step `LSR`/`BCC` ladder on `$01f5`; always observed idle (all-zero) so far |
| `01:f17d` | *(View-screen position stepper)* | Reads/adds/clamps `$7e21b4` (or writes `$e0` to `$7e21b5` on clamp), called from the View screen's direction-dispatch loop at `01:f0d3-f119` |
| `03:d255` | *(mode-select dispatch table)* | 20 entries, one per screen/menu mode selected by direct-page `$14` |
| `00:c0fb` | *(position-array initializer)* | Writes `0xE0` across an 8-slot, 4-byte-stride array starting at `$7e21b5` |

## D-pad patch sites (all fixed -- `src/main.c`, search for "dpad fix:")

Full reasoning for each in `docs/INVESTIGATION_dpad.md`. Table here is
just the address list for quick lookup.

| Site | Fix | Screen/context |
|---|---|---|
| `00:bb09`, `00:bc65`, `00:c220`, `01:a7b8`, `01:a7fd`, `01:ae2e`, `01:c132`, `01:c2f3` | `LDA $011b` -> `$011a` | Map scrolling, build cursor, toolbar, in-game menus (batch of 8) |
| `01:cc24`, `01:e8f4` | `LDA $c9(dp)` -> `$c8(dp)` | Mode-select and other list-style menus (edge-triggered) |
| `02:a50c`, `02:ab1f` | `LDA $ca(dp)` -> `$c9(dp)` | Tax modal |
| `03:d33e` | `LDA $ca(dp)` -> `$c9(dp)` | Mode-select (mode 3) |
| `03:d62a`, `03:ddc3`, `03:ddd9` | `LDA $ca(dp)` -> `$c9(dp)` | Scenario Select (mode 5), Save (mode 11) |
| `02:a4ef` | `AND #$fff0` -> `#$ffff` | Tax screen's per-frame cursor-update gate (widened, not shifted) |
| `00:d1b8` | `LDA $c9(dp)` -> `$c8(dp)` | Load/Save/Exit menu |
| `03:d3e2` | `LDA $ca(dp)` -> `$c9(dp)` | Map Select scenario-number picker |
| `03:dad9` | `LDA $0124` -> `$0123` | City-name-entry on-screen keyboard |
| `03:d97b` | `LDA $c9(dp)` -> `$c8(dp)` | Select-game-level (Easy/Medium/Hard) |
| `02:8525`, `02:9f37`, `02:9f43` | `LDA $011c` -> `$011b` | Comprehensive/Information map overlay (scroll + cursor) |
| `01:f0d3` | `LDA $011c` -> `$011b` | View screen (watch icon) |

## Compressed data regions (Nintendo LC_LZ5, see `tools/extract_graphics.py`)

| Region | ROM offset | Notes |
|---|---|---|
| Font tileset | `0x04C0FB` | 1bpp after reduction from the stored 2bpp form; ~10KB decompressed |
| Dialog text | `0x07A868`-`0x07DA83` | Plain ASCII on non-JP releases (no compression), one block |
| Scenario tileset | `0x04875C` | ~16KB decompressed |
| Scenario text, group 0 | `0x05BCAD`, 5 packets | |
| Scenario text, group 1 | `0x05EE30`, 12 packets | |

## Open investigation threads (see linked docs for full detail)

- **Cursor cadence** (`docs/REVERSE_ENGINEERING_cursor_movement.md`,
  `docs/INVESTIGATION_cursor_cadence.md`): real hardware steps `$01ed`
  every frame while held; this recomp only does so in ~4-frame-active/
  16-frame-idle bursts. Every individual branch condition along the call
  chain has been confirmed to evaluate identically to real hardware --
  the gap is in *how often the dispatcher itself gets invoked*, not a
  wrong decode. Leading candidate: an unconfirmed task-scheduler jump
  table in bank `0d`.
- **Fast travel** (`docs/INVESTIGATION_dpad.md`, "Open item: fast
  travel"): confirmed *not* implemented via the main cursor dispatcher's
  B/X/Y checks (those just set a reason code and return). Real location
  still unknown; `JSL $0098a0` remains a lead but has 90+ call sites.
- **View screen rendering** (`docs/INVESTIGATION_dpad.md`, "Open item:
  View screen's D-pad"): the write side is fully confirmed and working;
  no renderer/consumer of `$7e21b4`/`$7e21b5` has been found yet.
- **Sound**: no total-failure bug (audio active ~92% of a simulated
  minute); whatever's actually missing needs a specific description to
  chase further.
- **Widescreen** (`docs/PLAN_widescreen.md`): scoped, not implemented --
  the shared engine already has the rendering machinery; needs SimCity-
  specific BG-layer identification and visual verification.
