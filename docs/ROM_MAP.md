# SimCity (SNES, US) ROM map

> **CORRECTION:** entries below that describe `$011c`/`$ca`/`$0124` low
> nibbles as "hardware-guaranteed zero" and treat `AND #$0f00` direction
> checks as buggy were written against a runner defect, not the ROM. The
> auto-joypad halves were transposed (`$4218` = A/X/L/R + zero nibble,
> `$4219` = D-pad); fixed in snesrecomp b48daf4, compensating patches
> removed in 164a611. `LDA $011b (16-bit) / AND #$0f00` is the correct way
> to read the D-pad. Rows describing *what a routine does* remain valid;
> rows calling that routine buggy do not. See task #51.


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
| `03` | Mode-select dispatch table (`03:d255`, 20 entries) and its handlers: Map Select, city-name-entry keyboard, Select-game-level, Scenario Select; also the map-generation tile mask/copy loop (`03:cf82-cf9d`) |
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
| `$00d7`/`$d7` (dp) | **Not just a bail-out flag -- a dispatcher-selector state machine.** `01:8b3b-8b42` branches on it: `0` runs the "normal" per-frame cursor dispatcher (`01:8b4e+`, has the buggy Y-precondition described in the fast-travel writeup); `1` jumps to a second, near-identical dispatcher (`01:8c55`) with no such precondition. Also checked at `01:c0dd`/`01:948b` (role there still unconfirmed). Previously documented as "always 0 in every live sample" -- that was apparently just never sampled during actual fast-travel use | High (dispatcher-select role) / Medium (role at c0dd/948b) |
| `$0059`-`$005d` (`$59`/`$5b`/`$5d`) | Rolling checksum/hash accumulator pair, folds scenario parameters (`$0b27`-`$0b29`) via `00:824b`/`824f`; part of procedural map/seed generation (10-iteration loop at `03:d862`) | High (byte-level, live traced) |
| `$0079`/`$79` (dp) | Scratch: per-axis step delta in several contexts (cursor movement, decompressor table pointer) -- meaning is call-site-dependent | Medium |
| `$007a`-`$007e` | Scratch block used by a table-driven update loop at `00:94fd`-`95d8` (indexed via `$0b4d`) | Low, not traced in detail |
| `$007c`/`$7c` (dp) | **Reused scratch, not a single-purpose variable** -- among other uses, `00:cdec` treats it as a tight busy-loop delay counter (32 decrements within one frame); a red herring for the cadence investigation, see `docs/REVERSE_ENGINEERING_cursor_movement.md` | High (the "don't trust this" lesson is confirmed) |
| `$00c5`/`$c5` (dp) | "Reason code" written by the main-map cursor dispatcher when **A or Y** (`1`) is held (re-derived after the joypad-transposition fix: the gate `01:8bd6 AND #$4080` tests `$011b` bit 7 = A and `$011c` bit 6 = Y -- it is NOT B/X, which the pre-fix analysis wrongly concluded; measured holding Left, +A and +Y set `$c5`=1 while +B and +X do not), right before an early `RTS` (`01:8c52`). Consumer: `01:897f` reads it right after the write and dispatches via a jump table at `01:88ef` (`ASL A; TAX; JSR (table,X)`, opcode `0xFC`). Reason `1` (B/X) lands at `01:8d26`, which updates the animated hand-**cursor sprite** (OAM writes to `$7e2840+`/`$7e3040+`/`$7e3840+`) *and*, now that `01:8d36`'s direction-nibble read is fixed (see `01:8d26` below), correctly dispatches to a per-direction handler and calls `01:afbe`/`afc6` -- the actual map-scroll increment. **Fast travel confirmed fixed** -- see `docs/INVESTIGATION_dpad.md` "Fast travel" | High (dispatch mechanism itself, confirmed working end-to-end) |
| `$011b` | `$4218` mirror (held-state). **Per hardware this is the LOW half: bit7=A, bit6=X, bit5=L, bit4=R, bits0-3 = controller ID (always zero).** It does NOT hold the D-pad. Written by the shared edge-detector `00:928f-92cb` (`92c7: STA $011b,X`) from real `$4218,X` data, same call that populates `$c9,X`/`$0123,X`. NOTE: this row previously claimed the D-pad lived here -- that was a consequence of the runner transposing `$4218`/`$4219` (fixed, snesrecomp b48daf4); see the correction header in `INVESTIGATION_dpad.md` | High |
| `$011c` | `$4219` mirror (held-state). **Per hardware this is the HIGH half and holds the D-pad: bit7=B, bit6=Y, bit5=Select, bit4=Start, bit3=Up, bit2=Down, bit1=Left, bit0=Right.** A 16-bit `LDA $011b` puts this byte in the high half, so `AND #$0f00` tests Up/Down/Left/Right -- that idiom is correct ROM code, not a bug | High |
| `$011a` | One byte before `$011b`. Was the target of the removed "shift the load back one byte" patches, which existed only to compensate for the runner's transposed joypad halves. No longer used; kept here so the address is not mistaken for meaningful game state | High |
| `$0123`/`$0124` | Absolute (non-direct-page) mirror pair, same relationship as `$011b`/`$011c` | High |
| `$01bd`/`$01bf` | Map scroll-X / scroll-Y, **confirmed** (corrects the earlier `$01bd`/`$01be` guess -- no code anywhere in the ROM touches `$01be`; the real pair is 2 bytes apart). Clamped by `01:a0c4` against bounds in `$01c5`-`$01cb`; also the destination of the "warp to absolute tile coordinate" routine `01:a640`. No modifier-dependent step size found at either site -- still not the fast-travel mechanism | High |
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
| `$01eb`/`$01ed` | Also the target of the ported community mouse patch (see "Mouse patch integration" below) -- `src/main.c`'s `apply_mouse_delta()` writes here directly from host mouse motion when F3 is toggled on | High (write side) / Medium (which screen's cursor actually reads it) |
| `$0425` | **Debug-menu cheat flags bitfield.** `0x01`=No Disasters (inferred), `0x02`=Needless Money (**confirmed**, gates the deduction at `01:bb7a`), `0x04`=Valve Max (**confirmed**, gates the RCI-demand force at `03:8b37`), `0x08`=Water Reclaim (inferred), `0x10`=transient "Memory: SET selected" UI state (not a persisted cheat). Read at 19+ sites across banks 00/01/03. See "Debug menu / cheat mechanism" below | High |
| `$0429` | Currently-selected debug-menu option index (1-based); `00:da04`'s handler branches on it (`==0`->something else, `==5`->Memory commit, else->toggle) | Medium |
| `$0b9d`/`$0b9f` | City treasury/money, multi-byte value. Confirmed by the Needless-Money-gated deduction at `01:bbbc-bbcc` (`SEC; SBC $79; STA $0b9d` then an 8-bit borrow into `$0b9f`) | High |
| `$0bad`/`$0baf`/`$0bb1` | R/C/I demand-meter values ("valves"). Forced to `0x07d0`/`0x05dc`/`0x05dc` when Valve Max (`$0425 & 4`) is set (`03:8b37-8b4b`) | High |
| `$0200`-`$5fc0` | Raw/unmasked map tile buffer (24000 bytes, 12000 16-bit tiles), source for the mask/copy loop at `03:cf82-cf9d` | High |
| `$8000`-`$ddbf` | **Final, usable map tile buffer** (24000 bytes, 12000 16-bit tiles) -- each tile is `$0200+n AND $03FF`, written by `03:cf82-cf9d`. This is what a map-rendering/export tool should read; the raw buffer above still has flag bits (e.g. bit15 = 3x3 building footprint, per third-party RE notes, not independently verified) mixed into the tile ID | High (location/derivation) / Low (per-tile-value semantics) |
| `$0b4b`/`$0b4d`/`$0b4f` | Control values for the `00:94d5`-area table-driven update loop | Low |
| `$0bcb` | Written `0x0a` alongside `$01df=3` on the cursor dispatcher's interrupt path | Medium |
| `$0c0f` | The **other**, already-D-pad-fixed cursor-mover's own gate byte (`01:ae2e`'s routine) -- confirmed *not* the same mechanism driving `$01ed`'s cadence (stayed `0` throughout live cadence sampling) | High |
| `$0dc3` | Checked alongside the non-direction-button test at the very top of `01:8b4f` | Low |
| `$21b1` | Incremented by 2 alongside `$21b5` at `05:9c73` -- confirmed unrelated to the View screen, just a coincidental same-address match in unrelated bank-5 code (a plain read-modify-write self-increment, not a genuine consumer) | Low |
| `$21b4`/`$21b5` | View-screen D-pad-adjusted value (Left/Right and Up/Down respectively), stepped by `01:f189`/`f190` and `01:f19a`/`f1bf`. **Write-side bug fixed**: `$21b4` was always working correctly; `$21b5` was being stomped back to `$e0` every frame by `00:c0fb`'s array-init (see that entry) before `01:f1a7` (confirmed the *only* reader anywhere in the ROM, via a live memory watch across multiple screens/addressing modes) could ever see the real value. **But this is not the View screen's actual on-screen cursor**: these two bytes happen to sit at `$7e2000`+`0x1b4`/`0x1b5` -- the shadow-OAM buffer DMA'd to real OAM every frame (see `01:c0fb`'s DMA-channel-0 setup below) -- landing exactly on sprite #109's X/Y bytes, but that's a coincidence, not a connection: sprite 109 is confirmed idle (`tile=$00`, blank) and unrelated to the actual visible 4-tile icon (sprites 124-127, gated by a separate `$0b03` flag, `00:c189`) that stays frozen regardless of this value. What `$7e21b4`/`$7e21b5` actually feed into for rendering -- if anything -- is still unknown; see `docs/INVESTIGATION_dpad.md` "View screen's D-pad" for the full trace and why static analysis alone can't resolve this further (needs a live bsnes ground truth) | High (write-side bug, confirmed and patched) / Unknown (render connection, still open) |
| `$2000`-`$2220` (WRAM, i.e. `$7e2000`-`$7e2220`) | Shadow OAM buffer -- confirmed via `SC_GFX_TRACE`: DMA channel 0 (`00:8d7d`-`8d9b` sets it up: mode `$00`, dest `$2104`=OAM data port, size `$0220`=544 bytes) copies this *entire* range to real OAM every single frame. 128 sprites x 4 bytes (X, Y, tile, attribute), sprite N at `$2000+4N`. This is the destination for essentially all sprite positioning in the game -- e.g. sprite 109 = `$21b4`-`$21b7`, sprites 124-127 = `$21f0`-`$21ff` (a 2x2-tile icon) | High (DMA setup and range, confirmed live) |
| `$0b03` | "Icon N active" flag -- gates a per-~4-frame reset of sprites 124-127's Y to `$e0` (parking them) at `00:c189`: nonzero skips the reset. Set to `$ffff` by a function at `00:c624`/`c629` (also touches `$0aff`, an icon index) and cleared (with `$0aef`) by `00:c6d1`. Neither function's caller has been found (no direct `JSR` site in the ROM; likely an indirect/table dispatch not yet located) -- confirmed via live testing that neither fires during ordinary classic-map or View-screen play, so whatever feature activates this icon row wasn't exercised by any save state captured this session | Medium (mechanism confirmed) / Low (trigger, not found) |

## Named routines

See `docs/REVERSE_ENGINEERING_cursor_movement.md` for full pseudo-C and
confidence notes on each -- summary table only below.

| Address | Name | Purpose |
|---|---|---|
| `00:9278`/`00:927c` | *(edge-detector entry point)* | JSL wrapper (`9278`) into the real entry (`927c`), which sets `Y=4, X=0` before falling into `928f`'s loop -- processes all 4 SNES controller ports (`$4218`/`421A`/`421C`/`421E`, spaced by 2). What calls this each frame (and how often) is still unknown -- an exhaustive search for any reference to `9278` or a nearby candidate table (`01:8fda`, since retracted) found nothing; needs live tracing, not more static search |
| `00:928f-92cb` | *(shared edge-detector body)* | Busy-waits on `$4212 & 1` (auto-joypad-read-in-progress) before reading each port; XORs new vs. previous value for edge-detect, writes held/edge state to `$0123,X`/`$c9,X` **and** `$011b,X` (`92c7`) -- confirmed genuinely populates `$011b` with real data, contradicting a naive "always zero" read elsewhere (see `$011b` WRAM entry) |
| `01:afbe` | *(fast-travel scroll increment, FIXED and confirmed working)* | A 4-way `LSR`/`BCC` bit-ladder over `$01c1` (16-bit `LDA`, but only bits 0-3 tested): bit0 (Right) -> `afc6: INC $01bd`; bit1 (Left) -> `afcc: DEC $01bd`; bit2 (Down) -> `afd2: INC $01bf`; bit3 (Up) -> `afd8: DEC $01bf`. `$01bd`/`$01bf` are map scroll-X/Y. Called from the tail of `01:8d26` once `01:8d36`'s direction-nibble read is fixed (was the actual bug -- see `01:8d26`). Confirmed end-to-end via deterministic `--load-state`+`--input` testing: holding B+Right/B+Up reaches `afc6`/`afd8` respectively, and a before/after WRAM dump shows `$01bd`/`$01bf` genuinely changing |
| `00:90dd` | *(LC_LZ5 decompressor)* | Nintendo/community-named "LC_LZ5" compression; input bank/offset via WRAM `$09`/`$0b`, output to `$7E:0000+X`. See `tools/extract_graphics.py` for a verified-working reimplementation |
| `01:8b4f` | `CursorMoveDispatch_Frame` | Per-frame entry; checks non-direction buttons and a couple of state flags, bails to a reset path or falls into the mode-flag checker |
| `01:8b9f`/`8ba2` | `CursorMoveDispatch_CheckModeFlags` | Checks `$0395`/`$0383`/`$0387`, then B/X/Y held (writes `$c5` reason code + `RTS` if so), then falls into the direction ladder |
| `01:c132`/`c135` | *(direction ladder entry, already D-pad-patched)* | `LDA $011a; AND #$0f00` -- the fixed 16-bit direction test |
| `01:c155`/`c156` | *(the exact instruction gating the step, cadence investigation)* | 6th `ASL` of the direction word; the `BCC` here is what ultimately allows/blocks reaching `StepCursorAxis_*` |
| `01:c1ca` | `StepCursorAxis_Decrement` | `$01ed -= 2`, clamped at `0x18`, lands via `01:c1d5` |
| `01:c214` | `StepCursorAxis_Increment` | `$01ed += 2`, clamped at `0xd0`/`0xd1`, lands via `01:c221` |
| `01:c433`/`434` | `CheckAxisScrollBounds` | Per-direction delta lookup (`$0180c0,X`) and bounds pre-check against `$01eb`/`$01ed` |
| `01:c4c7`/`c4e5` | `DirectionPriorityScan_A`/`_B` | 4-step `LSR`/`BCC` ladder on `$01f5`; always observed idle (all-zero) so far |
| `01:f17d` | *(View-screen position stepper)* | Reads/adds/clamps `$7e21b4` (Left/Right) or `$7e21b5` (Up/Down, via `01:f19a`/`f1bf`), called from the View screen's direction-dispatch loop at `01:f0d3-f119` |
| `03:d255` | *(mode-select dispatch table)* | 20 entries, one per screen/menu mode selected by direct-page `$14` |
| `00:c0fb` | *(position-array initializer -- confirmed the View-screen D-pad bug's actual root cause, now patched)* | `SEP #$20; LDA #$e0; STA $7e21b5` (part of a loop at `00:8aa8` rebuilding a row of UI icon sprites into OAM via DMA), unconditionally writes `0xE0` across an 8-slot, 4-byte-stride array starting at `$7e21b5` -- runs every single frame, on every screen (confirmed also firing on the classic map, not View-specific). Confirmed live this stomps `01:f1bf`'s Up/Down write before `01:f1a7` (the array's *only* reader anywhere in the ROM) ever sees the update. Fixed in `src/main.c`: the one `STA $7e21b5` instruction is NOP'd out, leaving the other 7 slots untouched -- safe since nothing else reads that slot on any screen |
| `03:cf82-cf9d` | *(map tile mask/copy loop)* | `SEP #$20; PHB; LDA #$7e; PHA; PLB; REP #$30; LDX #0` loop: `LDA $7e0200,X (long); AND #$03ff; STA $8000,X; INX; INX; CPX #$5dc0; BNE`. Copies the raw generated map (`$7e0200+`) into the final masked buffer (`$7e8000+`), stripping flag bits from each tile |
| `01:88ef` | *(per-frame "reason code" jump table)* | Indexed by `$c5` (`ASL A; TAX; JSR (table,X)` at `01:897f`); entries found: `0`=no-op, `1`=`01:8d26` (cursor sprite), `2`=`01:8dce` (advisor toggle), `3`-`5`=`01:8e28`/`8e3d`/`9d6b` (menu-list auto-repeat, not traced in detail) |
| `01:8d26` | *(reason-1 handler -- fast-travel bug lived here, now FIXED)* | Reads a direction nibble at `01:8d36` (`LDA $011b` 16-bit `; SEP #$20 ; XBA ; AND #$0f`), dispatches to 4 per-direction OAM-sprite handlers (`b2f9`=Up/`b1f6`=Left?/`b166`=Down?/`b030`=Right? -- exact direction-to-handler mapping not individually confirmed beyond `b2f9`=Up) that animate the hand cursor, *and* (via the same nibble, stored to `$01c1`) feeds `01:afbe`'s scroll-increment ladder. **Root cause of the fast-travel bug**: the `XBA` before the 8-bit `AND #$0f` meant it tested `$011c`'s hardware-dead low nibble instead of `$011b`'s real direction bits -- a 9th, previously-unpatched site in the same bug family as the 8 `LDA $011b -> LDA $011a` sites fixed at load time (this one just has a different instruction shape, so the original byte-pattern scan missed it). Same fix applied: repoint the load's low byte to `$011a`. Confirmed fixed via deterministic testing -- see `docs/INVESTIGATION_dpad.md` "Fast travel" |
| `01:8dce` | *(reason-2/Y handler)* | Toggles `$01d7` and calls `JSL $0098a0` with inline param `6` (open) or `7` (close) -- reads as the advisor-panel toggle, matching the in-game tutorial text ("press Y" for advisor help) |
| `01:a0c4` | *(scroll-position clamp)* | Clamps `$01bd` to `[$01c7,$01c5]` and `$01bf` to `[$01c9,$01cb]` |
| `01:a640` | *(warp to absolute tile coordinate)* | Converts tile coords at `$0400`/`$0402` to scroll position via `01:a688`'s clamp, stores to `$01bd`/`$01bf` -- likely used for camera jumps (disaster alerts, advisor "take me there"), not incremental scrolling |
| `01:8c55-8c8e` | *(second reason-code dispatcher -- role now doubtful, see below)* | Near-identical to `01:8b4f`'s dispatcher, reached via `01:8b42`'s `$d7==1` branch instead of the default `$d7==0` path. Previously guessed (not confirmed) to be "the one that actually handles Y+direction (fast travel)" based on its Y-check (`01:8c75`) lacking the default dispatcher's `$01f5==0` precondition. **Now doubtful**: fast travel turned out to be fixable entirely within the default (`$d7==0`) dispatcher via B/X, not Y/A (see `01:8d26`/ROM_MAP "Fast travel"), and a deterministic scan of all 9 `STA $d7` sites in the ROM found none of them execute even once while holding Y+direction for 300 frames -- so `$d7` may simply never become 1 under these conditions, and this dispatcher's real trigger (if any) is still unknown |
| `01:9f2d` | *(reason-6 handler, dispatcher 2's Y-path)* | Extensive setup (clears `$01c1`/`$01f5`, sets several flags to `$ffff`, sets `$01df=3`), ends in `JMP $9dcc`. Internally also branches on `$d7` (`0`/`1`/`2` sub-states), separate from the top-level `$d7` dispatcher-select role |
| `01:9dcc` | *(possible task-scheduler entry point)* | Writes into tables at `$30c2,X`/`$ef20,X`/`$4420,X` indexed by `$01df` doubled, then returns immediately -- looks like "schedule a deferred task for mode `$01df`" rather than doing the work synchronously. Not yet confirmed as the same mechanism as the bank-`0d` task-scheduler lead from the cadence investigation, but a strong candidate -- worth checking |

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

## Debug menu / cheat mechanism (found via published Pro Action Replay codes)

A user-supplied list of published cheat-code addresses (Game Genie/PAR-style
`bank:addr:value`, same LoROM addressing this doc uses throughout) included
one labeled "Enable Debugger" at `01:88e7`. Tracing it forward, cross-checked
against a second, independent source (a fan guide crediting Corey Miller/
"ZaphodBee" describing an in-game debug menu reached via a controller-2 code
at the quit-confirmation screen), both point at the same mechanism:

- `01:88e7-88ee`: boot-time routine, `LDA $700009` (long -- reads one byte
  from cartridge SRAM) `; STA $0425 ; RTS`. The "Enable Debugger" PAR code
  replaces the load with `LDA #$80`, forcing this to look nonzero
  regardless of real SRAM contents.
- `00:da04-da3f`: the in-game menu's option handler. Looks up a per-option
  bitmask from a table at `00:da50` (`01 02 04 08 10 00` for options 1-6),
  XORs it into `$0425` to toggle (options 1-4), and for option 5
  ("Memory"), instead writes `$0425` back out to SRAM `$700009` -- the same
  address the boot loader reads. This is the "Memory: CLR/SET, reset to
  activate" flow the fan guide describes, fully confirmed from ROM bytes
  alone, no live testing needed.
- `$0425`'s individual bits are then read at 19+ sites across banks 00/01/03
  to gate the actual cheats -- see the `$0425` WRAM table entry above for
  the confirmed/inferred bit mapping.

`src/main.c` exposes this two ways: **F2** automates the documented
controller-2 entry sequence (unverified whether this ROM revision even
reads controller 2 -- a separate whole-ROM search for any `$421A`/`$421B`/
`$4016`/`$4017` access found none), and **F5-F8** poke `$0425`'s bits
directly, bypassing both the entry code and the in-game menu navigation
entirely. The direct-poke route is the higher-confidence one since two of
its four bits are independently confirmed against their actual consumers,
not just inferred from the option table's ordering.

## Open investigation threads (see linked docs for full detail)

- **Cursor cadence** (`docs/REVERSE_ENGINEERING_cursor_movement.md`,
  `docs/INVESTIGATION_cursor_cadence.md`): real hardware steps `$01ed`
  every frame while held; this recomp only does so in ~4-frame-active/
  16-frame-idle bursts. Every individual branch condition along the call
  chain has been confirmed to evaluate identically to real hardware --
  the gap is in *how often the dispatcher itself gets invoked*, not a
  wrong decode. Leading candidate: an unconfirmed task-scheduler jump
  table in bank `0d` -- possibly the same mechanism responsible for the
  ~4-frame periodic gating found via the fast-travel thread below, but
  an attempted static identification of that scheduler (a table at
  `01:8fda`) didn't hold up on closer inspection and was retracted --
  not yet resolved either way.
- **Fast travel: FIXED.** Root cause was a 9th, previously-unpatched site
  in the same D-pad "wrong nibble" bug family as the 8 sites already
  fixed at load time: `01:8d36` does `LDA $011b` (16-bit) `; SEP #$20 ;
  XBA ; AND #$0f ; BEQ ...`. The `XBA` swaps A's bytes before the 8-bit
  `AND`, so it tested `$011c`'s hardware-dead low nibble instead of
  `$011b`'s real direction bits -- meaning the `BEQ` always took, so
  `01:8d26` (reached whenever B or X is held, via `$011b AND #$4080` at
  `01:8bd6`) never wrote `$01c1` and never dispatched to any of its 4
  per-direction handlers (`b2f9`/`b1f6`/`b166`/`b030`), for *any* held
  direction. Fixed with the same technique as the other 8 sites (repoint
  the load's low byte from `$011b` to `$011a`). Confirmed end-to-end via
  deterministic testing (a user-captured save state + `--load-state` +
  `--input <frame>:<dur>:<mask>`, holding B+Up/B+Right with zero live-input
  jitter): the direction handlers now fire, `01:afbe`'s bit-ladder now
  runs with real data, and a before/after WRAM dump shows `$01bd`/`$01bf`
  (map scroll-X/Y) genuinely changing. Zero regression on the full
  10800-frame qualify baseline. The earlier "scheduling race" theory
  (`$011b` reading `$0000` at `01:c01e`'s periodic checks) turned out to
  be a live-keyboard-timing artifact, not a real bug: the same
  deterministic test shows `$011b`/`$011c` read correctly at `01:c01e`
  on every single hit once input is held via `--input` instead of a
  physical key. Also corrects an earlier mislabeling: the modifier this
  mechanism actually checks (`$011b AND #$4080`) is **B or X**, not Y/A --
  `$011c` bit 6 is X, not Y (Y is `$011b` bit 6); Y/A do not reach this
  code path at all (confirmed: `$c5` only ever became `0`/no-op or `2`
  /advisor-toggle while holding Y+direction, never `1`). See
  `docs/INVESTIGATION_dpad.md` "Fast travel" for the full writeup and the
  save-state-based testing method that finally cracked it.
- **View screen rendering** (`docs/INVESTIGATION_dpad.md`, "Open item:
  View screen's D-pad"): the write side is fully confirmed and working;
  no renderer/consumer of `$7e21b4`/`$7e21b5` has been found yet.
- **Sound**: no total-failure bug in the underlying DSP simulation (audio
  active ~92% of a simulated minute, confirmed via headless qualify
  mode). Live testing found a real, specific symptom: audio in the
  windowed build lags behind by 1-2 seconds, traced to fast-forward
  batches (`frames_this_iter > 1` in `src/main.c`) leaving several
  frames' worth of DSP output undrained each time, which then plays back
  later as increasingly stale audio -- this includes the *automatic*
  fast-forward (not just Tab-held), which fires any time the LC_LZ5
  decompressor runs, including ordinary dialog/UI popups mid-game, not
  just loading screens. Two attempts to discard that backlog (both a
  hand-rolled fix and the shared runner's own `dsp_trimSamples()`) each
  caused a complete, permanent audio freeze in live testing instead --
  root cause of *that* not found, reverted. The 1-2s delay remains
  unfixed; `SC_AUDIO_DEBUG` (periodic drain-loop stats) was added for the
  next attempt.
- **Widescreen** (`docs/PLAN_widescreen.md`): scoped, not implemented --
  the shared engine already has the rendering machinery; needs SimCity-
  specific BG-layer identification and visual verification.
