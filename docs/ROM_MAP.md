# SimCity (SNES, US) ROM map

> **CORRECTION:** entries below that describe `$011c`/`$ca`/`$0124` low
> nibbles as "hardware-guaranteed zero" and treat `AND #$0f00` direction
> checks as buggy were written against a runner defect, not the ROM. The
> auto-joypad halves were transposed (`$4218` = A/X/L/R + zero nibble,
> `$4219` = D-pad). **That diagnosis was also wrong**: the runner is correct
> and the bug was this host's own `kPad_*` bit order, now fixed in
> `src/main.c` with the submodule reverted to stock. See the README's D-pad
> section. Compensating ROM patches removed in 164a611. `LDA $011b (16-bit) / AND #$0f00` is the correct way
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

Code lives in `00`-`03` and `05`. `04` and `06`-`0f` are data.

| Bank(s) | Contents |
|---|---|
| `00` | Core engine: boot, NMI/joypad handling, shared edge-detector (`00:928f-92cb`), LC_LZ5 decompressor (`00:90dd`), various shared utilities |
| `01` | Shared UI/cursor dispatch code: map cursor movement, mode-select ladder, direction-priority scanners, the `01:8b4e`+ per-frame dispatcher chain |
| `02` | Modal screens: Tax, Save, and similar bank-2-resident popups |
| `03` | Mode-select dispatch table (`03:d255`, 20 entries) and its handlers: Map Select, city-name-entry keyboard, Select-game-level, Scenario Select; also the map-generation tile mask/copy loop (`03:cf82-cf9d`) |
| `04` | **Data, not code.** 0% executed across three recorded play sessions. A byte scan finds 72 `22 xx xx 04` sequences that look like `JSL` into this bank, but only six lie in code banks at all, all six target `04:8f7e` (which is **entirely zero-filled**), and none of the six ever executed -- so they are data coincidences and dead bytes, not calls. The bank's content is regularly-structured bitplane-looking data (e.g. `02 03 05 06 3f 1f 7c 0f`) in ~`$100`-spaced records |
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
| `$0059`-`$005d` (`$59`/`$5b`/`$5d`) | **PRNG state** (two 16-bit words plus a temp), stepped by `00:824f`. Seeded from the map seed `$0b27`-`$0b29` at `03:d840`. Previously described here as a "checksum/hash accumulator" -- see the `00:824b` routine entry for why that was wrong | High (byte-level, live traced) |
| `$0b27`-`$0b29` | **Map seed**, three bytes. `03:d840` mixes them into the PRNG state and `03:d873` copies them to `$0b2a`-`$0b2c`, i.e. the generated map is fully determined by these three bytes | High |
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
| `00:8211` | *(COP syscall dispatcher)* | `CLI ; PHB ; PEA $0000 ; PLB ; PLB ; REP #$20 ; REP #$10 ; ASL A ; TAX ; JSR ($8223,X) ; PLB ; RTI`. An 11-entry service table at `00:8223`, service number in `A`, ~309 call sites ROM-wide. Table: 0/5/6 -> `930d`, 1 -> `86a4`, 2 -> `8ea9`, 3 -> `8e43`, 4 -> `8e75`, 7 -> `9479`, 8 -> `90dd` (LC_LZ5), 9 -> `8f82`, 10 -> `86c8`. Confirmed live by bsnes trace (`A=4` dispatched to `008e75`, `A=0` to `00930d`) |
| `00:930d` | ***(COP service 0 -- wait for vblank; the LLE-scheduler yield primitive)*** | `SEP #$20 ; STZ $b9 ; INC $c7 ; LDA $b9 ; BEQ -6 ; RTS`. Spins until the NMI handler releases it with `INC $b9` at `00:80bc` (gated on bit 7 of `$00b1`, which service 4 at `00:8e75` sets). 133 call sites -- the most-used service. **This is SimCity's once-per-frame quiescence point**, i.e. the "which PCs are the yield primitives" answer `snesrecomp/docs/LLE_SCHEDULER.md` asks each game for. Unlike Mega Man X's coroutine-switch yield, this one plainly returns via `RTS` |
| `$00c7` | *(spin counter / PRNG seed source)* | Incremented once per spin iteration while `00:930d` waits for vblank, and read by `00:823e` to seed `$59`/`$5b`/`$5d` -- so the PRNG is seeded from how long the player took, which is what makes generated maps vary |
| `00:824b` / `00:824f` | ***(PRNG step -- NOT a checksum)*** | `CLC ; LDA $59 ; STA $5d ; ADC $5b ; STA $59 ; ADC $5d ; STA $5b ; RTS` -- an additive (lagged-Fibonacci-with-carry) generator over two 16-bit state words, returning the new `$5b` in `A`. It takes **no input**, which is what rules out a checksum: it folds nothing in, it only advances state. Earlier notes here and in the README called it a "shared checksum/hash routine"; the seeding at `03:d840` does fold `$0b27`-`$0b29` in, but that happens once, outside this routine. Proof it is used as randomness: `01:f1fd` calls it and immediately does `AND #$00ff ; CMP #$0056 ; BCS`, i.e. branches on a ~34%/66% split of the returned byte |
| `03:d840`-`03:d889` | *(map-generation seeding)* | `LDA $0b28 ; EOR #$ffff ; ROL A x5 ; ADC #$1238` seeds `$5b`; a second mix of all three seed bytes gives `AND #$001f -> X`, then `JSL $00824b ; DEX ; BPL` runs the PRNG **1-32 times, a seed-dependent count** (not the fixed 10 iterations previously recorded). Then `JSL $01f1ed` and `JSL $02923f` do the actual work |
| `01:f1ed` / `01:f1f1` | *(terrain feature generator)* | Draws a random byte from `00:824b` and branches: roughly a third of the time `JSR $f22c`, otherwise a chain of five distinct feature routines (`$f380`, `$f5b9`, `$f311`, `$f444`, `$f3a3`). This is the core evidence that map generation is genuinely procedural rather than a table of prebuilt maps |
| `02:923f` / `02:9243` | *(generation scratch clear + upload)* | Zero-fills `$7EA400`-`$7EBFFF` (7168 bytes) then sets up DMA -- the rendering/upload side of generation |
| `00:90dd` | *(LC_LZ5 decompressor)* | Nintendo/community-named "LC_LZ5" compression. Input bank/offset via WRAM `$0b`/`$0009`; output to **`$7E8000 + X`**, with `X` loaded from `$000e` (16-bit, so the output window reaches `$7F7FFF`) -- *not* `$7E:0000+X` as this row previously said. Reached via `COP #$00` with `A = 8`. `00:926d` handles a source bank crossing by setting `Y = $8000` and incrementing the data-bank register. See `tools/extract_graphics.py` for a reimplementation verified byte-exact against a live run, and `docs/REFERENCE_map_format.md` for the full decode |
| `03:b0e5` | *(power grid: SET a cell's bit)* | `JSR $b120 ; SEP #$20 ; LDA $7fa598,X ; ORA $b0dd,Y ; STA $7fa598,X` -- ORs one bit into the packed power bitmap. `$03b0dd` is the 8-entry mask table `80 40 20 10 08 04 02 01`, MSB-first, matching the order `03:b152` consumes it in |
| `03:b0f8` | *(power grid: TEST a cell's bit)* | Same indexing, `AND $b0dd,Y`, result returned in `Y`. `CPX #$05dc` bounds it at **1500 bytes** -- independent confirmation that the bitmap is 12000 bits, one per map cell. Two tile values are special-cased before the lookup (`$0b89` compared against `$027c` and `$028c`) and answer "powered" unconditionally -- i.e. self-powered buildings. Counting them in the decoded maps supports that: every built scenario contains a handful (Bern 5, Boston 3+2, Detroit 3+2, Rio 9, San Francisco 7, Tokyo 8, Las Vegas 5) while **free play and the tutorial, the two terrain-only maps with no buildings at all, contain none of either**. Consistent with the two power-plant types; which of the pair is coal and which is nuclear is not established |
| `03:b245` / `03:b258` | *(power grid: traversal stack)* | A push/pop pair over two parallel arrays. `INC $0c13 ; LDX $0c13 ; LDA $0b85 -> $0c15,X ; LDA $0b86 -> $0c34,X` and the matching pop with `DEC $0c13`. `$0c13` is the depth, `$0b85`/`$0b86` the working cell coordinate being saved across a branch of the walk. Called from `03:b358` (push) and `03:b206` (pop) -- i.e. the flood fill that propagates power out from the plants and fills the bitmap |
| `$0c13` | *(power traversal stack depth)* | see `03:b245` |
| `$0c15`+, `$0c34`+ | *(power traversal stack)* | two 16-entry parallel arrays; among the hottest WRAM in the game |
| `03:9035` | *(shared bank-03 utility, 30+ call sites)* | Entry is `03:9035`, not `03:9040`: `PHP ; REP #$20 ; PHD ; TDC ; SEC ; SBC #$0006 ; TCD` allocates a **6-byte direct-page frame**, so the `$00`/`$02` it then uses are frame locals, not absolute addresses. It shifts the 6-slot 16-bit window `$0ccf`-`$0cdc` up by one (`LDA $0ccd,X ; STA $0ccf,X`, X = 12 down to 2) while accumulating, stores the total at `$0ccf`, then calls the inline-operand helper below. Called from 30+ sites across bank 03. What the window holds is not established |
| `03:a2f5`, `03:a350`, `03:a3cf`, `03:a421` | ***(inline-operand helper family)*** | Four routines sharing one calling convention, 61 call sites between them (a2f5: 34, a421: 18, a3cf: 8, a350: 1). **The emitted AOT code for these call sites is wrong** -- see `docs/UPSTREAM_inline_args.md` |
| `03:a3cf` | ***(inline-operand helper -- a calling convention, not a routine)*** | `REP #$30 ; PLA ; TAY ; CLC ; ADC #$0003 ; PHA` -- pops its own return address into `Y`, advances it past **three inline bytes**, and pushes it back so the eventual `RTS` skips them. It then allocates an 8-byte DP frame and uses `LDA $0001,Y` / `LDA $0002,Y` -- those inline bytes -- as **indices into the caller's direct-page frame** (`LDA $08,X`). So a call site reads `JSR $a3cf` followed by three operand selectors; `03:9063` passes `00 02 04`. A compact bytecode over bank 03's shared math layer |
| `03:b152` | ***(power writeback scan)*** | Allocates a 2-byte direct-page frame (`PHD ; TDC ; SEC ; SBC #$0002 ; TCD`), sets `DB = $7f`, then walks all 12000 map cells: every 16th cell fetches the next word of a **packed power bitmap at `$7FA598`** (`LDA $a598,Y ; XBA ; STA $00`), and per cell does `LDA $0200,X ; AND #$7fff ; ASL $00 ; BCC +3 ; ORA #$8000 ; STA $0200,X` -- i.e. clear the power bit, shift the next bitmap bit into carry, re-set it if set. `CPX #$5dc0` bounds it to the 24000-byte map. Found by a bsnes write breakpoint on `$7F0200`; this is the routine that makes bit 15 mean "powered" |
| `$7FA598` | *(packed power bitmap)* | One bit per cell, 12000 bits = 1500 bytes, consumed MSB-first by `03:b152`. **Not part of the SRAM save block** (which restores only `$7F5FC0` and `$7F6560`), which is the root cause of the post-load power dropout: after a load this bitmap has to be recomputed from scratch before anything reads as powered |
| `03:ddba` | *(scenario-select: the Las Vegas / free-play gate)* | `LDA #$02 ; LDX $42 ; BPL +1 ; INC A ; STA $79`. `$79` is the maximum column index: 2 normally, 3 when bit 15 of the completion bitfield is set. Because `X` is 16-bit, `LDX $42` sets `N` from bit 15 -- the "all six scenarios beaten" flag -- so beating the six is literally what widens the grid by one column. This is the whole unlock |
| `03:ddd9` | *(scenario-select: cursor + index)* | Left/Right move `$52` (column), clamped against `$79` at `03:dde4` and against 0 at `03:ddf3`; `$54` is the row. Scenario index is `row * 3 + column` (`03:de0e`), except column 3, which special-cases to **6** on row 0 and **7** on row 1 (`03:de04`). Result is stored to `$40` at `03:de1a` -- the same byte `03:ce3c` reads to pick the map pointer. `03:de27` sets the smooth-scroll target `$22` to `$50` for column 3, animated into `$16` at `03:de31` |
| `03:ded0` | *(scenario-select: win-mark drawing)* | Walks the 8 bits of `$42` with `LSR`/`BCC`, drawing sprite `$29` for each completed scenario at coordinates from two 8-entry tables: `$03df20` -> `$025f` (row: `$014`, `$06c`) and `$03df30` -> `$025d` (column: `$00e`, `$05e`, `$0ae`, `$0fe`, minus the scroll offset `$16`). The layout is therefore **4 columns x 2 rows**, with the six scenarios in columns 0-2 and Las Vegas / free play alone in column 3 at `$0fe`, past the right edge of a 256-wide screen |
| `03:e2ee` | *(scenario completion / "win mark" setter)* | Gated on `$3e == 3` and `$0d87 == 2` (win). `03:e30a` ORs a mask from the table at `03:e334` (`0001, 0002 … 0080`, indexed by scenario x2) into the direct-page word `$42`; once the low **six** bits are all set, `03:e31c` also sets bit 15, the game's own "every scenario beaten" flag. `03:e326` commits `$42` to SRAM `$700007`. The fall-through vs. branch paths differ only in `X` (a message index, 10 vs 11), i.e. a different congratulation when the last scenario completes |
| `03:e360` | *(SRAM init)* | Loads `$700007` into `$42` (`03:e36c`) and `$700009` into `$0425` (the cheat flags). Measured: none of this executes at all in 3600 frames from a cold boot -- the SRAM subsystem is only reached through a real game session |
| `03:e411` / `03:e42d` | *(SRAM header verify)* | Checksum loop summing the **bytes** `$700000`-`$70000d` into a 16-bit total compared against `$70000e`, then a `'S'`,`'I'`,`'M'` magic test at `$700000`-`$700002`. A header failing either is restored from the backup copy at `$707ff0` (`03:e446`) |
| `03:e553` | *(SRAM header commit)* | Recomputes the same checksum into `$70000e`, then `JSR $e484` mirrors the 16-byte header to `$707ff0`. Any host-side edit of the header must do both or be silently reverted -- see `apply_unlock_all()` in `src/main.c` |
| `03:ce8b` | *(per-scenario seed)* | Five 8-entry word tables indexed by `$0040`: `$03cec9` -> `$0c0d`, `$03ced9` -> `$0b53` (**scenario year**: 1906/1965/1961/1972/2010/2047/2096/1991), `$03cee9` -> `$0deb`+`$0ca5` (city class), `$03cef9`/`$03cf09` -> `$0ba5`/`$0ba7` (starting population). Note these are 8 entries where the *map pointer* table at `03:ce70` is 9 |
| `03:ce2e` | *(scenario map loader)* | Reads the scenario index from `$0040`, pulls a 24-bit map pointer out of the 9-entry struct-of-arrays table at `03:ce70`/`ce79`/`ce82`, decompresses it to `$7E8000`, then `JSR $d15f` to unpack |
| `03:d15f` | *(scenario map unpacker)* | Three further stages -- word-level LZ (`03:d16c`), run-length expansion (`03:d1c0`), and a zero-fill plus 3x3 building-stamp walk (`03:d1fb`/`03:d210`) -- producing the live 24000-byte, 120x100 map at `$7F0200`. Fully decoded in `docs/REFERENCE_map_format.md`; `tools/extract_maps.py` reimplements the whole chain |
| `05:9304` | *(boot map/WRAM blob copy)* | `MVN $7f,$7e` moving 32768 bytes `$7E8000` -> `$7F0000`; paired with a second at `05:9329` for `$7F8000`. Together with the two decompressions that feed them (`0d:d77c` and `0e:c242`) this fills the whole `$7F` bank at boot, before any scenario is chosen |
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

## Screen-mode dispatch (`$14`) -- and how to force any screen

`03:d289` is `LDA $14 ; ASL ; TAX ; JSR ($d255,X)`: the direct-page byte `$14`
selects a screen handler from a 23-entry table at `03:d255`. (Note `03:d286` is
*mid-instruction* -- the operand of a `REP #$20` -- so tracing that address
catches nothing, which is easy to mistake for "the dispatcher never runs".)

| `$14` | handler | | `$14` | handler |
|---|---|---|---|---|
| 0 | `03:d2b8` | | 12 | `03:df40` |
| 1 | `03:d2c6` *(boot/attract loop sits here)* | | 13 | `03:e1ec` |
| 2 | `03:d304` | | 14 | `03:e246` |
| 3 | `03:d333` | | 15 | `03:e257` |
| 4 | `03:d388` | | 16 | `03:e292` |
| 5 | `03:d3ca` | | 17 | `03:e296` |
| 6 | `03:d88a` | | 18 | `03:d30f` |
| 7 | `03:d8bb` | | 19 | `03:e2df` |
| 8 | `03:d951` | | 20 | `03:e344` |
| 9 | `03:d964` | | 21 | `03:d9eb` |
| 10 | `03:dd52` | | 22 | `03:da26` |
| 11 | **`03:ddb6` scenario select** | | | |

**`SC_FREEZE=14:<mode>` forces any of these to run**, which largely removes the
recurring "there is no save state at that screen" blocker: the handler executes
and its logic can be traced and its WRAM read, without navigating menus or
hand-timing `--input`. Graphics may be wrong (the preceding mode's setup never
ran), but the *logic* is real.

Worked example -- confirming the Las Vegas unlock end to end, from a cold boot,
with no save state at all:

```
SC_FREEZE=14:0b        SC_ADDR_TRACE=03:ddc1   ->  a=0002, x=0000  ($79 = 2)
SC_FREEZE=14:0b,43:80  SC_ADDR_TRACE=03:ddc1   ->  a=0003, x=8000  ($79 = 3)
```

i.e. freezing bit 15 of the completion field really does widen the grid by a
column, observed executing rather than argued from the disassembly. Freezing
`$43` directly also side-steps needing formatted SRAM, which a cold boot does
not have.

## WRAM usage map (from a recorded play session)

`SC_WRAM_MAP=<file>` records, per WRAM byte, whether it was read/written, the
**last** PC to write it, and a saturating write count. Measured over a session
that built a city, went bankrupt, lost a scenario, took a loan and collected
gifts:

- **Only 2,410 of the 8,192 bytes of `$7E0000`-`$7E1FFF` are live** -- i.e. ever
  written again after boot. The other 5,782 are written once by the boot clear
  and never touched. Most of the "variable space" is unused.
- `00:8018`-`00:8037` is that boot clear (`STA $00,X ; INX ; DEY ; BNE`, then a
  second loop over `$7E2000`+). A byte still owned by it is dead space, so
  last-writer is only meaningful where the owner is *not* this routine.
- `$1F00`-`$1FFF` is the stack. `$1E80`-`$1EFF` is a **relocated direct page**:
  several bank-03 routines do `PHD ; TDC ; SEC ; SBC #$0002 ; TCD` to allocate
  DP locals (e.g. `03:b152`, the power scan), so hot bytes there are stack
  frames, not variables.

### Auto-repeat: `$012b,X`, `$0133`, `$0135`

The shared edge detector at `00:929b` maintains a **fourth** per-port array
nobody had documented, and it implements key auto-repeat:

```
LDA $4218,X ; EOR $011b,X ; AND $bf   ; newly-pressed edges
STA $c9,X   ; STA $0123,X
CMP previous held
  changed   -> $012b,X = $0133        ; initial delay
  unchanged -> DEC $012b,X
               on zero: STA $0123,X   ; re-post held state as a fresh edge
                        $012b,X = $0135   ; repeat rate
```

`05:92a2` sets the constants once at boot: `$0133 = 10` frames initial delay,
`$0135 = 4` frames repeat. Confirmed live in save states.

**This does NOT pace the main-map cursor**, which was worth testing rather than
assuming. Holding Right from a loaded map with `$0133`/`$0135` frozen to 1
produces a byte-identical trajectory to the stock values (cursor `$01eb`
128 -> 138 -> 178 -> 218 -> clamp, then scroll `$01bd` +5 per 20 frames). The
cursor steps 2 units *every frame* and reads the **held** state (`$011b`/`$011c`)
directly, so auto-repeat only governs consumers of the **edge** array
(`$0123,X`) -- menus and list navigation. `docs/INVESTIGATION_cursor_cadence.md`'s
conclusion about the map cursor stands.

### Moving-object table, `$02cb`-`$0376`

A **column-major** entity table -- the same struct-of-arrays idiom as the
scenario map pointers at `03:ce70`, and just as invisible to a constant scan.
Field arrays are 16 bytes apart, each holding ~6-8 entries of 2 bytes, indexed
by `Y`. Updated by a family of routines at `03:f26a`-`03:f3e1`, one per field.

`03:f32d` shows the shape: `$0317,Y` is a per-entity countdown (`DEC`), `$0327,Y`
a **signed byte** velocity (sign-extended via `BMI` -> `ORA #$ff00`), and
`$0337,Y` a 16-bit position it accumulates into. `03:f3d0` does the same for
`$02f7`/`$0307` sourced from a ROM table at `$03eb71`. Consistent with the
game's moving objects (vehicles/aircraft/disaster sprites); which entity is
which is not established.

### Other live blocks with a single owner

| block | owner | notes |
|---|---|---|
| `$028b`-`$029a`, `$029b`-`$02aa` | `01:c8a5`, `01:c84b` | two 16-byte UI blocks, very hot |
| `$0c16`-`$0c25`, `$0c35`-`$0c44` | `03:b251`, `03:b257` | 16 bytes each, simulation side |
| `$0cd1`-`$0cdc` | `03:9053` | 12 bytes, saturated write count |
| ~250 bytes | `01:c891`/`01:c896` | largest single UI-owned region |
| ~224 bytes | `00:8aba`/`00:8ac1` | the OAM icon-row rebuild loop (`00:8aa8`) |

`03:a390`-`03:a3ce` is a **32-bit software multiply** (shift-and-add: 32
iterations of `ASL $0c ; ROL $0e ; ROL $10 ; ROL $12` with a conditional
`ADC`), writing its result to `$14,X`/`$16,X`/`$18,X`/`$1a,X` on a relocated
direct page. It is one of the hottest routines in the game -- the simulation's
arithmetic workhorse.

## Cartridge SRAM layout (`$700000`+)

SRAM is **not** part of `g_ram` -- it lives in the cart model, so WRAM dumps do
not capture it and it has to be read back through the bus. `SC_SRAM_DUMP_PATH`
dumps the 32KB window and prints a decoded header line.

A 16-byte header, then the per-city save block. The header is checksummed and
mirrored, so a host-side edit that updates only the field it cares about will
be reverted at the next verify -- see `03:e411`/`03:e553` above.

| Address | Meaning |
|---|---|
| `$700000`-`$700002` | Magic `'S'`,`'I'`,`'M'` |
| `$700007` (16-bit) | **Scenario completion bitfield.** Bit N = scenario N won (mask table `03:e334`); bits 0-5 the six ordinary scenarios, bit 6 Las Vegas, bit 15 = "all six beaten", set by the game itself at `03:e31c`. Read into `$42` at init |
| `$700009` | Debug-menu cheat flags, mirrored to `$0425` |
| `$70000e` (16-bit) | Checksum: sum of the **bytes** `$700000`-`$70000d` |
| `$700010`-`$700084` | Per-city save fields, copied one by one to WRAM by `03:c8f1` (load) / `03:cc2e` (save). Includes `$700036` -> `$0deb` (city class) and `$700070` -> `$0040` (scenario index) |
| `$700084` +60 bytes | Array -> `$0ced` (30 words) |
| `$7000c0` +1440 bytes | Array -> `$7f5fc0`, i.e. directly after the 24000-byte map |
| `$700660` +1440 bytes | Array -> `$7f6560` |
| `$707ff0`-`$707fff` | Backup copy of the 16-byte header (`03:e484` writes it, `03:e446` restores from it) |

Verified against real game-written SRAM (an in-game save state): magic reads
`SIM` and the stored checksum matches a recomputation exactly.

Two save-file bases beyond the first appear at a stride of `$3ff0` (`$703ff0`
and `$707ff0`); `03:e392`-`03:e402` reads the same field set from two of them,
which looks like a save-slot summary. Not investigated further.

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

## Simulation tick, calendar, seasons, population and the annual budget

Recovered with `tools/dis_mx.py` (width-tracking disassembly cross-checked
against the coverage bitmap) plus `SC_WRAM_MAP` write-attribution on a save
state whose city is actually running. Every listing below is fully
`*`-marked — i.e. every instruction quoted was executed in a recorded
session — and no operand byte is marked executed, so the widths are the ones
the CPU used.

### The tick routine, `03:8000`

Entered with `SEP #$20 ; REP #$10`, then `LDA #$03 ; PHA ; PLB` to put DB=3.
It calls a fixed pipeline (`$90a7`, `$c474`, `$b84b`, `$88b4`, `$894c`,
`$821d`, `$8297`, `$addf`) and then advances time:

```
03:8026  INC $0b51                  ; tick counter
03:8029  LDA $0dc7 ; CLC ; ADC $0dc5 ; STA $0dc7
03:8033  LDA $0b51 ; AND #$0003 ; BNE $80b0     ; every 4th tick only:
03:803b  INC $0b55                  ; month
03:803e  LDA $0b55 ; CMP #$000d ; BNE $804f
03:8046  LDA #$0001 ; STA $0b55     ; month wraps 13 -> 1
03:804c  INC $0b53                  ; year
```

| address | meaning |
|---|---|
| `$0b51` | tick counter. Cadence depends on the in-game speed setting — 200 frames per tick on one save state, 125 on another running faster |
| `$0b55` | month, 1..12 |
| `$0b53` | year (1902/1904/1905/1991/… matching the per-scenario seed at `03:ced9`) |
| `$0dc7` | accumulator, `+= $0dc5` every tick |

So **4 ticks = 1 month** and **12 months = 1 year**. That structure is fixed;
the tick *cadence* is not, because the game has a speed setting. On one state
a tick took 200 frames (verified over 9,000 frames: 45 ticks, 1904 → 1905);
on a state running faster, six months passed in 3,000 frames, i.e. 125 frames
per tick.

`$0b53 - 10` is stored to `$0da9` and `$0b55 - 1` to `$0dad` (display
forms), and in month 1 also `$0b53 - 120` to `$0dab`.

### Seasons: two month-indexed tables at `03:8160` and `03:816d`

```
03:8090  LDY $0b55
03:8093  SEP #$20
03:8095  LDA $8160,Y ; BEQ $80b0        ; gate: only on a season boundary
03:809a  LDA $816d,Y ; CMP $0b4d ; BEQ $80b0
03:80a2  STA $0b4d                      ; new season
03:80a7  LDA #$0001 ; STA $0b4b         ; "season changed" flag
03:80ad  STZ $0b4f
```

Both tables are bytes indexed directly by month (index 0 unused):

```
03:8160 gate    00 00 00 01 00 00 01 00 00 01 00 00 01 03
03:816d season  03 03 03 00 00 00 01 01 01 02 02 02 03 03
```

The gate is nonzero only at months **3, 6, 9 and 12**, and `$0b4d` becomes
0, 1, 2, 3 there. So the seasons are Mar–May = 0, Jun–Aug = 1, Sep–Nov = 2,
Dec–Feb = 3. Confirmed dynamically: over 9,000 frames `$0b4d` was written
exactly **4 times**, all from `03:80a5`.

### Population, `03:8196`

```
03:81a3  LDA $0b8f ; CLC ; ADC $0b93
03:81aa  ASL A ; ASL A ; ASL A          ; x8
03:81ad  ADC $0b8b
03:81b0  STA $00
03:81b2  LDA #$0014 ; STA $02           ; x20
03:81b7  JSR $a2f5  [00 02 00]          ; 16x16 -> 32 multiply, inline args
03:81bd  LDA $00 ; STA $0ba5            ; population, low word
03:81c2  LDA $02 ; STA $0ba7            ; population, high word
03:81c7  $0de3:$0de5 = population - $0bcd:$0bcf     ; change since last
```

**`population = (($0b8f + $0b93) * 8 + $0b8b) * 20`**, held as a 32-bit
value in `$0ba5` (low) / `$0ba7` (high).

### Which zone each tally counts

Three sibling accumulators at `03:924f`, `03:92fb`, `03:93b1` each call a
helper that turns the tile index in `$0b89` into a capacity contribution.
The helpers differ only in the class base they subtract:

| helper | tile base | cycle | returns | tally |
|---|---|---|---|---|
| `03:842f` | `#$0099` = 153 | `#$24` = 36 | `Y * 8` | `$0b8b` |
| `03:8456` | `#$0144` = 324 | `#$2d` = 45 | `Y` | `$0b93` |
| `03:847a` | `#$0201` = 513 | `#$24` = 36 | `Y` | `$0b8f` |

Each subtracts its base, returns 0 if the tile is below it, reduces modulo
the cycle and then divides by 9 — i.e. 4 development levels of 9 tiles for
residential and industrial, 5 for commercial.

In tile-index order (153 < 324 < 513) that is **residential, commercial,
industrial**:

| tally | zone |
|---|---|
| `$0b8b` | **residential** |
| `$0b93` | **commercial** |
| `$0b8f` | **industrial** |

Two independent checks agree. The residential helper applies its own `* 8`
where the other two get it in the population formula, so all three end up in
the same units. And in a real city residential dominates: savestate 2 holds
`$0b8b` = 2718 against `$0b93` = 102 and `$0b8f` = 225.

The formula was verified against save states directly — exact on every state
where the tick has actually run (s2: 106,680; s6: 15,320). States that differ
are scenarios still holding the starting population seeded from `03:cef9` /
`03:cf09`, before the first recomputation.

`SC_WRAM_MAP` attributes the two stores to `03:81BF` / `03:81C4`, written 45
times in 45 ticks — population is recomputed every tick.

### The annual budget, `03:8df1`

Called from the tick **only when `$0b55 == 1`** (`03:8087`), i.e. once per
game year.

```
03:8dfe  STZ $0bc1
03:8e01  LDA $0dc3 ; BEQ $8e07 ; RTS    ; re-entrancy guard
...
03:8ec8  LDA #$0001 ; STA $0dc3
03:8ece  LDA $0dc3 ; BNE $8ece          ; spin until the UI clears it
03:8ed6  LDY $0b1d ; BEQ $8ee1
03:8edb  DEC $0b1d ; LDA #$01f4         ; 500 charged while $0b1d counts down
03:8ee2  ADC $0dcf ; ADC $0dd1 ; ADC $0dcd
03:8eeb  STA $00                        ; $00 = total outgoings
03:8eed  LDA $0dc9 ; CLC ; ADC $0dd9 ; SEC ; SBC $00
03:8ef7  STA $0bc1                      ; net balance for the year
03:8efa  treasury($0b9d:$0b9f) += $0dc9:$0dcb, += $0dd9, -= $00
03:8f27  LDA $0b9d ; CMP #$423f ; LDA $0bff ; SBC #$000f ; BCC $8f41
03:8f35  clamp to $000F423F
```

| address | meaning |
|---|---|
| `$0b9d` / `$0b9f` | **treasury, 32-bit**, clamped to `$000F423F` = **999,999** |
| `$0dc9` / `$0dcb` | annual income, 32-bit |
| `$0dd9` | further income term, added separately |
| `$0dcd`, `$0dcf`, `$0dd1` | three outgoing line items |
| `$0bc1` | net balance for the year |
| `$0dc3` | budget-dialog busy flag; set to 1, then spun on until the UI clears it |
| `$0b1d` | counts down; while nonzero, **500 per year** is added to outgoings |
| `$0dd5`, `$0dd7` | derived stats, each clamped to `#$270f` = 9999 |

Three outgoing line items against one tax income is the shape of the game's
budget screen. **`$0b1d` is the bank loan** — confirmed by the user from
play — so `$0b1d` is the number of annual repayments outstanding and 500 is
the yearly instalment.

### Gift-building income: `03:ae61`, `$0ddd` -> `$0dd9`

`$0dd9`, the income term added to the treasury separately from taxes, is a
straight copy of `$0ddd` at `03:8e95`. `$0ddd` is zeroed at `03:8279` and
accumulated one building at a time by `03:ae61`, which is a flat comparison
ladder on the tile index:

```
03:ae61  INC $0c71                       ; count of paying buildings
03:ae64  LDY #$012c ; CMP #$02fe ; BEQ   ; 300/year
03:ae6c  LDY #$00c8 ; CMP #$02ec ; BEQ   ; 200/year
03:ae74  LDY #$0064                      ; 100/year for any of:
         CMP #$02f5 / #$034f / #$033d / #$0334 / #$032b / #$0319
03:ae95  LDY #$0000                      ; everything else pays nothing
03:ae98  $0ddd += Y
```

| payout per year | tile indices |
|---|---|
| 300 | `$02fe` |
| 200 | `$02ec` |
| **100** | `$02f5`, `$0319`, `$032b`, `$0334`, `$033d`, `$034f` |

This matches the reported behaviour that **a casino pays $100 per year**, so
the casino is one of the six tiles in the 100 group. Which building each of
the eight tile indices is has not been established — that wants placing them
in play and watching `$0c71`, not guessing from the ROM.

The `$0dc3` spin at `03:8ece` also explains why the treasury never moves in
an unattended replay: the routine parks there until the budget dialog is
dismissed, so a headless run never reaches the arithmetic. `$0b9d` was
written zero times across a full simulated year.

## The `$c5` reason-code dispatch, `01:897f`

```
01:897f  LDA $c5 ; REP #$10 ; ASL A ; TAX ; JSR ($88ef,X)
```

12 word entries at `01:88ef`, bounded by `01:8907` being its own node. What
each handler *is* is only partly established; the table below separates what
was read from the code from what is confirmed.

| `$c5` | handler | what it does |
|---|---|---|
| 0 | `01:8d25` | bare `RTS` — **idle / no-op state** |
| 1 | `01:8d26` | early-out on `$01f5`, then `JSR $8aa8` and `$01c1` — cursor sprite + direction dispatch (previously established) |
| 2 | `01:8dce` | branches on `$01d7`, clears `$01ff` |
| 3 | `01:8e28` | clears `$0249`, then a second dispatch on `$020d` (`ASL A ; TAX`) |
| 4 | `01:8e3d` | `JSR $8e9b` with a carry result, `JSR $b42a`, also reads `$020d` |
| 5 | `01:9d6b` | `JSR $b143`, then `LDA #$0000 ; COP #$00` — **waits for vblank** via COP service 0 |
| 6 | `01:9f2d` | clears `$0379`, branches on `$d7 == 2` |
| 7 | `01:c529` | early-out on `$01f5`, then reads `$c9` |
| 8 | `01:93a8` | copies `$0111` -> `$0117` and `$03fa` -> `$03fc` (double-buffered UI state) |
| 9 | `01:93a4` | `JSR $a640` then `RTS` — a one-line wrapper that falls into 8's neighbourhood |
| 10 | `01:940f` | **the annual budget dialog** — see below |
| 11 | `01:94e6` | `JSR $9790`, `JSR $9c9b`, clears `$0383` |

`$020d` is a sub-selector shared by reasons 3 and 4.

### Reason 10 closes the budget handshake

The annual budget at `03:8ec8` sets `$0dc3 = 1` and then spins:

```
03:8ec8  LDA #$0001 ; STA $0dc3
03:8ece  LDA $0dc3 ; BNE $8ece      ; parks here until the UI clears it
```

Reason code 10 is the other half. `01:9419` reads `$0dc3` and tests its sign
(`BPL`), gates on `$d7`, `$0195` and `$01d7`, and on the accept path clears it:

```
01:945e  STZ $0dc3
```

`STZ $0dc3` also appears at `02:a311`, `02:a3e5` and `03:c7ba`; `03:8ebd` and
`03:8ecb` are the two stores that raise it. This is why the treasury never
moves in an unattended replay — nothing dismisses the dialog, so `03:8ece`
never releases and the arithmetic after it is never reached.

## `$0195` — the four in-game option toggles

The options screen keeps all four settings as bits of one word at `$0195`,
flipped by the `EOR` at `01:a9c1` (inside `01:a97c`, one of the five `$01df`
UI state handlers). Each bit is tested with its own mask:

| bit | mask | option | test sites | confidence |
|---|---|---|---|---|
| 0 | `$0001` | **auto bulldozing** | `01:bab4`, `01:bad4` | **confirmed** |
| 1 | `$0002` | **auto budget** | `01:9422`, `03:8ec0` | **confirmed** |
| 2 | `$0004` | **auto goto** | `01:8b82`, `01:8c96` | confirmed by elimination |
| 3 | `$0008` | **music on/off** | `00:8087`, `00:c8b1` | strong |

The bit order is the order the options appear on the menu page.

Confirmed against save states captured with known settings:

| state | settings as set in-game | `$0195` |
|---|---|---|
| bulldoze on, budget off, goto on, music on | | `$000d` = `1101` |
| bulldoze off, budget on, goto off, music off | | `$0002` = `0010` |
| **then bulldozing turned back on**, budget still on | | `$0003` = `0011` |
| budget turned off, bulldozing left on | | `$0001` = `0001` |

Turning bulldozing on moved `$0002` -> `$0003`, so **bit 0 is auto
bulldozing**; turning auto budget off moved `$0003` -> `$0001`, so **bit 1 is
auto budget**. Goto and music were only ever changed together in this set, so
bits 2 and 3 are not separated by the states alone — but bit 3 is read in
bank 00's audio setup in 8-bit mode and feeds an `#$81` command byte, which
makes music bit 3 and auto goto bit 2 by elimination, matching the menu
order.

"Auto goto" teleports the view to the event — a traffic jam, for instance —
which is consistent with bit 2's site at `01:8b8a` selecting view mode
`#$0009` in place of `#$00ff`.

Bit 0 sits in the build path and gates on the tile index: `01:babd` compares
against `#$002e` = 46, so the bit permits building over tile classes below
that — terrain, parks, forest, rubble. Bit 2 selects `#$0009` instead of
`#$00ff` as a view/overlay mode (`01:8b8a`). Bit 3 is read in 8-bit mode
during bank 00's audio setup, choosing a value that is then passed with an
`#$81` command byte.

Only bit 1 has been confirmed dynamically; the other three are read from
context. Toggling each option in play and reading `$0195` would settle all
four in one session.

### Auto budget, exactly

```
03:8eba  LDA #$ffff ; STA $0dc3        ; default: negative
03:8ec0  LDA $0195 ; AND #$0002 ; BEQ $8ece
03:8ec8  LDA #$0001 ; STA $0dc3        ; auto budget: positive
03:8ece  LDA $0dc3 ; BNE $8ece         ; spin either way
```

and in reason code 10:

```
01:9419  LDA $0dc3 ; BPL $9467         ; positive -> skip the dialog
...
01:9467  JSL $02a3dc ; JSL $02a64d ; BRA $945e
01:945e  STZ $0dc3                     ; both paths land here
```

So the sign of `$0dc3` is the channel: `$ffff` means "ask the player" and `1`
means "allocate automatically". The manual path shows the dialog via
`COP #$00` service 3 at `01:945a`; the automatic path runs two bank-02
routines and falls into the same clear.

### What was observed with auto budget forced on

Freezing `$0195` to `$02` and replaying savestate 5 for 12,505 frames does
reach the annual budget: `$0dc3` is raised at `03:8ebd`/`03:8ecb`, the spin
releases when `02:a3e8` clears it, and the income terms are computed —
`$0dc9` (tax) written from `03:8eb2`, and **`$0dd9` = 100**, i.e. exactly one
gift building paying 100 a year, which is the reported casino payout observed
live rather than read from the ROM.

That first attempt did **not** reach the treasury update, because freezing
`$0195` to `$02` also forces the other three options off, which is not a
state the game ever produces.

### Verified end to end

Replaying a save state captured in **December with auto budget genuinely
enabled** rolls the year over and runs the whole thing unattended:

| term | address | value |
|---|---|---|
| tax income | `$0dc9` | 151 |
| gift income | `$0dd9` | 0 |
| outgoing 1 | `$0dcd` | 88 |
| outgoing 2 | `$0dcf` | 100 |
| outgoing 3 | `$0dd1` | 0 |
| net | `$0bc1` | `$FFDB` = **-37** |
| treasury | `$0b9d`/`$0b9f` | 2994 -> **2957** |

`151 + 0 - (88 + 100 + 0) = -37`, `$0bc1` holds -37 as a signed word, and the
treasury moves by exactly that. `$0b9d` was written once, from `03:8F1F` —
the store at `03:8f1c` that had until now only been read from the ROM. The
model is confirmed against a running machine.

The three outgoing line items are written by **`02:a65f`, `02:a665` and
`02:a66b`**, inside the `JSL $02a64d` that reason code 10 calls on the
automatic path. So bank 02 holds the funding allocator, and `$0dc3` was
cleared from `01:9461` as predicted.

### The loan instalment, verified

A second state — December 1903, auto budget on, **loan outstanding** — closes
the last term. Replaying it across the year rollover:

| term | address | value |
|---|---|---|
| tax income | `$0dc9` | 604 |
| gift income | `$0dd9` | 0 |
| outgoings | `$0dcd`/`$0dcf`/`$0dd1` | 88 + 100 + 0 = 188 |
| loan instalment | — | 500 |
| net | `$0bc1` | `$FFAC` = **-84** |
| treasury | `$0b9d`/`$0b9f` | 10196 -> **10112** |
| loan counter | `$0b1d` | 21 -> **20** |

`604 + 0 - 188 - 500 = -84`. `$0b1d` was written exactly once, from
`03:8EDE` — the `DEC $0b1d` at `03:8edb` — so one instalment is charged and
one repayment retired per year, as read.

The complete annual equation is therefore

```
treasury += (tax + gift) - (out1 + out2 + out3) - (500 if $0b1d != 0)
$0b1d    -= 1 while nonzero
```

with the treasury then clamped to `$000F423F` = 999,999.

### `$0193` — the game speed

The third settings word, edited by UI page 0 (`01:a886`). Two save states
identical but for the speed setting differ in exactly this byte:

| in-game speed | `$0193` |
|---|---|
| 3/3 | 0 |
| 1/3 | 2 |

so the field counts *down* from fastest; 2/3 = 1 and the menu's 0/3 = 3 follow
by implication but were not captured. `$79`, the menu selection byte, mirrors
it while the page is open.

Both states also tick **zero times in 2,400 frames**, which confirms
separately that the modal handler pauses the simulation while a settings page
is open — the `COP`/poll loop at `01:a886` never returns to the tick.

## The `$01df` UI state machine — five menu handlers

`$01df` selects through two parallel tables, `01:9d1a` (called) and
`01:9d3a` (jumped), at `01:a8e9` and `01:a8f8`. The five handlers share one
shape — a modal loop that waits a frame and polls input:

```
LDA #$0000 ; COP #$00      ; service 0, wait for vblank
JSR $ae26                  ; sample
JSR $aecc                  ; handle; returns carry set when done
BCC <loop>
```

| `$01df` | handler | what it edits |
|---|---|---|
| 0 | `01:a886` | reads `$0193`, calls `$a918` first |
| 1 | `01:a97c` | **the options screen** — reads `$0195` |
| 2 | `01:aa39` | reads `$0197`, writes `$79` |
| 3 | `01:aad5` | plain modal loop, then `CMP #$0008` |
| 4 | `01:ad54` | plain modal loop |

`$0193`, `$0195` and `$0197` are three parallel settings words, one per page.

### `$0195` really does hold exactly four option bits

`01:a97c` renders the options page with

```
01:a97e  LDA $0195      (8-bit)
01:a981  ASL A ; ASL A ; ASL A ; ASL A
01:a985  XBA
```

Four shifts then `XBA` lifts **bits 0-3** into the high byte for the menu
renderer, so the option nibble is exactly four bits wide. That is an
independent confirmation that the four masks found elsewhere (`$0001`,
`$0002`, `$0004`, `$0008`) are the complete set, and matches the four options
the game actually offers.

`01:aa39` does the same with two shifts on `$0197`, so that page carries a
two-bit field, and stores the result to `$79` — the same selection byte the
scenario-unlock work writes.

## `03:a553` — the map cell-pattern rewriter

Found empirically: it is in the 128 addresses that first executed in a session
where a tornado was allowed to run its course, against a union of eight
recorded sessions. 109 of those 128 were in bank 03.

```
03:a553  LDY #$0000
03:a556  LDA $00 ; CLC ; ADC $a6e2,Y ; TAX      ; cell + signed offset
03:a55d  LDA $7f0200,X ; CMP $a6f0,Y ; BNE $a585 ; must match the pattern
03:a566  INY ; INY ; CPY #$000e ; BNE $a556      ; seven cells
03:a56d  <second loop>
03:a577  LDA $a6fe,Y ; STA $7f0200,X             ; write the replacement
03:a585  LDA #$0001 ; RTS
```

Two passes over three parallel 7-entry word tables: **verify all seven cells,
then rewrite all seven**. Nothing is written unless the whole shape matches,
which is what makes it a structure transform rather than a per-tile edit.

| table | contents |
|---|---|
| `$a6e2` offsets | `fe1c fe1e ff0e fffe 00ee 01de 01dc` |
| `$a6f0` expected | `035c 035d 0001 0355 0001 035b 035a` |
| `$a6fe` replacement | `0001 0031 0031 0031 0031 0031 0001` |

The map is at **`$7F0200`**, 120 cells per row at 2 bytes each = 240 bytes per
row, which agrees with the 120x100 layout in `REFERENCE_map_format.md` and
with the address the post-load power fix pokes. The offsets are signed and
decode to a coherent shape:

| offset | rows | cells |
|---|---|---|
| `$fe1c` = -484 | -2 | -2 |
| `$fe1e` = -482 | -2 | -1 |
| `$ff0e` = -242 | -1 | -1 |
| `$fffe` = -2 | 0 | -1 |
| `$00ee` = +238 | +1 | -1 |
| `$01de` = +478 | +2 | -1 |
| `$01dc` = +476 | +2 | -2 |

A five-row vertical strip. The expected tiles are all in the `$03xx` range —
above the industrial zone base of 513, so special/gift structures, the same
band as the buildings in the annual-income ladder — and they are replaced with
`$0001` and `$0031`, i.e. the structure is levelled.

Immediately after, `03:a589` shows the trigger shape:

```
03:a589  CMP #$0354 ; BNE                       ; only for this tile
03:a58e  JSR $907e ; AND #$0003 ; BNE $a585      ; 1-in-4 random
03:a596  JSR $a70c ; CMP #$0015 ; BCC $a585      ; threshold 21
03:a59e  <another offset/expected/replacement triple at $a6b8/$a6c6/...>
```

so `03:907e` is a random source and `03:a70c` yields a value tested against
21. Several such triples sit consecutively in `$a6b8`-`$a70c`.

### Two arms, selected by tile id

The caution above turned out to be right: this is a **table-driven** rewriter
with one arm per structure tile, not a tornado routine. Both arms have the
identical shape —

```
03:a53b  LDA $0b89 ; CMP #$0355 ; BNE $a589     ; arm for tile $0355
03:a543  JSR $907e ; AND #$0003 ; BNE           ; 1-in-4 draw
03:a54b  JSR $a70c ; CMP #$0015 ; BCC           ; threshold 21
03:a553  <verify 7 / rewrite 7 against $a6e2/$a6f0/$a6fe>

03:a589  CMP #$0354 ; BNE                       ; arm for tile $0354
03:a58e  JSR $907e ; AND #$0003 ; BNE
03:a596  JSR $a70c ; CMP #$0015 ; BCC
03:a59e  <verify 7 / rewrite 7 against $a6b8/$a6c6/$a6d4>
```

so `$0b89` — the current tile index, the same variable the zone-tally helpers
decode — picks the pattern. The two triples are 42 bytes each, laid out
consecutively:

| tile | offsets | expected | replacement |
|---|---|---|---|
| `$0354` | `$a6b8` `ff0a fffa fffc fffe 0000 0002 ff12` | `$a6c6` `0356 0357 0001 0354 0001 0359 0358` | `$a6d4` `0001 0030 0030 0030 0030 0030 0001` |
| `$0355` | `$a6e2` `fe1c fe1e ff0e fffe 00ee 01de 01dc` | `$a6f0` `035c 035d 0001 0355 0001 035b 035a` | `$a6fe` `0001 0031 0031 0031 0031 0031 0001` |

The `$0354` offsets are a compact horizontal cluster; the `$0355` offsets span
five rows vertically. Both keep two `$0001` cells as anchors and level the
other five to `$0030` or `$0031`. That reads as the two orientations of one
multi-tile structure, each with its own rubble tile.

This matches the reported behaviour that **the monster does the same thing as
the tornado with different tiles** — the event picks the tile, the tile picks
the table, and the rewriter is shared. It also means enumerating the rest of
the triples would enumerate the destructible structures directly.

### The other map writers during a disaster

Attributing every map-cell write during a tornado replay:

| writer | cells |
|---|---|
| `03:B191` | 19,826 |
| `03:A53A` | 3,474 |
| `03:82F3` | 562 |
| `03:99B5` | 126 |
| `03:84EA` | 12 |

`03:a536` (recorded as `03:A53A`, the instruction after) is a separate,
narrower edit — `AND #$ff0f ; ORA $0b41 ; STA $7f01fe,X` — rewriting the low
nibble of a cell's attribute byte from `$0b41` rather than replacing the cell.

### Determinism

Reloading a save state reproduces the disaster exactly — same location, same
damage — confirmed in play. The recompilation is deterministic on this path,
which is what makes these states usable as regression fixtures.

**The AOT tier is verified on this code.** All four tornado states replayed
900 frames on both tiers give **byte-identical 128 KB WRAM**. That extends the
COP and inline-argument verification onto code that had never executed in any
earlier recording.

### A replayable disaster dataset

Unlike the earlier six, save states captured *during* a running tornado do
advance under headless replay (tick 11 -> 12, 20 -> 22, 33 -> 35 over 900
frames), so a disaster in progress can be stepped and diffed. The earlier
"armed but not yet fired" states all sat in `$01df = 2` and never ticked.

## The monster step, `03:bb6a`, and what it drags in

Isolated by difference: a session in which the monster rampaged executed 36
addresses that nine sessions — including the tornado run — never had. Only two
regions, `03:bb6a-bbb8` and `03:b92e-b93c`.

### The per-tick gate, `03:b92e`

```
03:b92e  JSR $907e ; AND #$0007 ; CMP #$0002 ; BCS $b93e
03:b939  JSR $bb6a                 ; two chances in eight
```

### The step itself

```
03:bb6c  LDA #$0014 ; STA $00
03:bb71  JSR $bc9f ; TAY           ; pick a random map cell, tile id -> Y
03:bb75  LDA $84eb,Y ; AND #$0001 ; BNE      ; per-tile property table, bit 0 = skip
03:bb7d  CPY #$0088 ; BCC          ; only tiles >= $88
03:bb82  LDA #$007f ; STA $7f0200,X          ; stamp tile $7F over the target
03:bb8b  LDA $04 ; STA $0400 ; LDA $05 ; STA $0402   ; remember where
03:bb97  INC $03fe
03:bb9a  LDA #$0020 ; JSR $be04    ; post event $20
03:bba0  LDA #$000b ; JSR $c42a    ; allocate entity type $0B
03:bba6  INC $0c9f
03:bbab  LDA #$21 ; STA $0006
```

### Four things this identifies

**`03:9035` is a bounded random number generator**, not the smoothing window
an earlier note guessed at. `03:bc9f` is simply "pick a random cell":

```
03:bc9f  LDA #$0077 ; JSR $9035 ; STA $04    ; x in 0..119
03:bca7  LDA #$0063 ; JSR $9035 ; STA $05    ; y in 0..99
03:bcaf  LDA $04 ; JSR $849e                 ; (x,y) -> cell index
```

`#$0077` = 119 and `#$0063` = 99 are exactly the 120x100 map bounds, which
also confirms `03:849e` as the coordinate-to-cell-index helper.

**`$0ced` is the moving-object table.** `03:c42a` scans it for a free slot:

```
03:c42a  PHA ; LDX #$0000
03:c430  LDA $0ced,X ; CMP #$ffff ; BEQ <found>
03:c438  TXA ; CLC ; ADC #$0006 ; TAX ; CPX #$003c ; BNE
```

stride 6, limit `$3c` = 60, free marker `$ffff` — **10 slots of 6 bytes**. The
monster is **entity type `$0B`**. This is the entity table listed as an open
thread; the identities can now be read off from each caller's type byte.

**`03:be04` is a one-shot event post**, guarded so only one is pending:

```
03:be04  LDY $0395 ; BNE $be11 ; STA $0397 ; INC $0395
```

The monster posts event `$0020`.

**`$84eb` is a per-tile property table**, indexed by tile id, with bit 0
meaning "not a valid target".

### The sound is still unattributed

The reported difference — the monster makes sounds, the tornado does not —
looked like it would fall out of `JSR $c42a`, but that is the entity
allocator, and `03:be04` is an event post. Neither touches the APU directly.
The sound most likely follows from the entity or the event downstream rather
than from the step, and is not established here.

## The real disaster dispatcher: `03:b8ae` on `$0197`

Not `$0199`. `03:b8ae` is a six-arm ladder over **`$0197`**, each arm calling a
handler and then masking its own bit off:

| bit | mask | handler | clears with |
|---|---|---|---|
| 0 | `$0001` | `03:bbb9` | `$fe` |
| 1 | `$0002` | `03:bc0b` | `$fd` |
| 2 | `$0004` | `03:b9cd` | `$fb` |
| 3 | `$0008` | `03:b9db` | `$f7` |
| 4 | `$0010` | `03:baf5` | `$ef` |
| 5 | `$0020` | `03:ba47` | `$df` |

```
03:b8ae  LDA $0197 ; BEQ $b916          ; nothing pending -> ordinary path
03:b8b3  AND #$0001 ; BEQ ; JSR $bbb9 ; LDA #$00fe ; BRA $b90e
         ... one arm per bit ...
03:b90e  AND $0197 ; STA $0197          ; clear the bit just serviced
```

**`$0197` is also the third settings word** — the page `01:aa39` renders. That
page is the disaster-selection menu: choosing a disaster sets its bit, and
this ladder fires the handler and clears it. An earlier note called `$0197` a
"two-bit field" because `01:aa3b` shifts it left twice before `XBA`; that was
wrong. Two shifts then `XBA` lifts **six** bits into the high byte, exactly as
four shifts lift the four option bits of `$0195`.

### Which are attributed, and how much is still dark

Per-handler coverage over nine recorded sessions, with the two sessions that
deliberately ran one disaster each broken out:

| bit | handler | body executed | tornado session | monster session |
|---|---|---|---|---|
| 0 | `03:bbb9` | 31 / 82 | — | — |
| 1 | `03:bc0b` | 4 / 8 | — | 4 |
| 2 | `03:b9cd` | 4 / 14 | — | — |
| 3 | `03:b9db` | 41 / 108 | **41** | — |
| 4 | `03:baf5` | 47 / 117 | — | — |
| 5 | `03:ba47` | 71 / 174 | 19 | **63** |

So **bit 3 is the tornado and bit 5 is the monster**, attributed because those
sessions ran one disaster deliberately. Bits 0, 2 and 4 have partial coverage
from ordinary play — disasters fire on their own — but nothing says which is
which. Roughly **60% of the six handlers' code has still never executed.**

Attributing the rest needs one session per disaster, each triggering a single
type, so that session's newly executed addresses name its bit the way the
tornado and monster runs did.

### Reading the unattributed arms

> **SUPERSEDED — bit 2 is the plane crash, not the meltdown.** See "Bit 2 is
> the plane crash" further down: `$0a8d` counts **airports**, not nuclear
> plants, and the meltdown is not in the `$0197` ladder at all. The paragraph
> below is kept because its *mechanism* reading is right — the arm really is
> just a guard on `$0a8d`, which is why it shows so few covered bytes — only
> the identification was wrong. Left in place rather than deleted so the
> correction stays visible; the table further down carries the right answer.

**Bit 2 (`03:b9cd`) is the nuclear meltdown.** Its whole body is a guard:

```
03:b9cf  LDA $0a8d ; BEQ $b9da ; ... ; RTS
```

`$0a8d` is the nuclear-plant count — incremented at `03:ac08` when one is
built, decremented at `03:ce47` (never executed in any recording). Measured
across the save states: it is **0 in the Boston state with the plants deleted
and 1 in the Boston states that still have one**, which is exactly the
reported behaviour that removing every nuclear plant removes the disaster.
This is why the arm shows only 4 of 14 bytes covered — in most recordings it
takes the early exit.

**Bit 0 (`03:bbb9`) is a roaming destroyer, like the monster but pickier.**
Same shape as `03:bb6a` — random cell, stamp tile `$7F` — with two
differences: the strength parameter is `#$0028` (40) rather than `#$0014`
(20), and the target must have **property bit 2** set in `$84eb` as well as
not having bit 0:

```
03:bbc4  LDA $84eb,Y ; AND #$0001 ; BNE <skip>    ; never a target
03:bbcc  LDA $84eb,Y ; AND #$0004 ; BEQ <skip>    ; must have this property
```

A per-tile "may catch fire" flag is the natural reading, which would make this
the fire, but that is inference from the shape of the test, not evidence.

**Bit 4 (`03:baf5`) is the earthquake** — confirmed by a session that
triggered only that: 47 of its 117 bytes ran, matching the union exactly,
while bits 0, 2 and 3 stayed at zero. It starts at a stored location rather
than a random one, so `$0ba9`/`$0baa` is the **epicentre**:

```
03:baf7  LDA $0ba9 ; AND #$00ff ; STA $0400
03:bb00  LDA $0baa ; AND #$00ff ; STA $0402       ; a remembered coordinate
03:bb0c  LDA #$000a ; JSR $be04                   ; event $0A
03:bb12  LDA #$000e ; JSR $c42a                   ; entity type $0E
03:bb18  LDA #$015e ; JSR $9035                   ; random 0..350
```

`$0ba9`/`$0baa` is written at `03:9ba0`, `03:9bb8` and `03:c859`, and holds
(56,56) and (60,50) in the captured states. An event that begins somewhere
specific rather than anywhere fits several candidates; it is not settled.

**Bit 1 (`03:bc0b`) is 148 bytes, not 8.** An earlier note here said 8 and
called it a shared tail; that was an artefact of measuring each handler's span
as the distance to the next handler *in the order I happened to list them*
rather than in address order. `03:bc0b` runs to `03:bc9f`, where the
random-cell picker starts. Corrected spans are used in the table below.

### Status

| bit | handler | span | identification |
|---|---|---|---|
| 0 | `03:bbb9` | 82 | roaming destroyer, needs tile property bit 2 — unplaced |
| 1 | `03:bc0b` | 148 | unplaced; runs in most sessions, so not disaster-specific |
| 2 | `03:b9cd` | 14 | **plane crash** — `$0a8d` is the airport count; see the correction below |
| 3 | `03:b9db` | 108 | **tornado** — confirmed by session |
| 4 | `03:baf5` | 117 | **earthquake** — confirmed by session; epicentre `$0ba9`/`$0baa`, entity `$0E` |
| 5 | `03:ba47` | 174 | **monster** — confirmed by session, entity `$0B` |

Reported but not yet placed: **flood** and the **UFO**, which appears in the
Las Vegas scenario rather than in ordinary play. Six bits for
more candidates than that means at least one reported event is not driven by
this ladder — fire spreading tile-to-tile rather than being dispatched once
would be the obvious way that happens.

## `$0b57` — difficulty, and what it actually changes

`$0b57` reads 0 or 1 across every captured save state and does not track city
size (`$0ca5`/`$0deb` do that — both 3 in an 80,000-population city where
`$0b57` is 0). It indexes two tables, in the two places difficulty is
reported to matter.

### Disaster frequency — confirmed and quantified

```
03:b91e  LDA $0b57 ; ASL A ; TAY
03:b923  LDA $b969,Y ; JSR $9035      ; random 0..N
03:b929  CMP #$0000 ; BNE <skip>      ; proceed only on a zero draw
```

`03:9035` is the bounded RNG, so the per-tick chance is **1 in (N+1)**. The
table at `03:b969` holds three sane entries before running into unrelated
bytes, which is what fixes its length at three:

| `$0b57` | N | chance per tick |
|---|---|---|
| 0 | 4800 | 1 in 4801 |
| 1 | 2400 | 1 in 2401 |
| 2 | 1200 | 1 in 1201 |

So each difficulty step **doubles** the disaster rate, 4x from easiest to
hardest. That confirms the reported behaviour that medium and hard throw far
more disasters than easy, and puts a number on it.

### The tax claim does not hold up as stated

The other difficulty-indexed table is at `03:8fe8`, used in the annual budget:

```
03:8e3c  LDA $0e17 ; ASL A ; ADC $0e15 ; STA $00
03:8e45  LDA $0b57 ; ASL A ; TAX ; LDA $8fe8,X ; STA $04
03:8e4f  JSR $a2f5  [00 04 00]        ; multiply
```

with `$8fe8` = `00b3 00e6 0133 ...` = **179, 230, 307**. The multiplier
*increases* with difficulty, which is the opposite direction to "taxes are
lower on higher difficulty". Either this term is not the tax rate — it is fed
by `$0e15`/`$0e17` and multiplied, so it could as easily be a cost or demand
factor — or the widely repeated claim is wrong.

Worth stating plainly because the source was second-hand: the disaster half of
that claim is confirmed in the ROM, the tax half is not, and nothing here
settles which reading of `$8fe8` is right. Watching `$0dc9` (tax income) across
a year on two difficulties with an otherwise identical city would settle it.


### Per-arm coverage by single-disaster session

The attribution method, laid out so it can be checked:

| bit | handler | span | union | tornado | monster | quake | flood |
|---|---|---|---|---|---|---|---|
| 2 | `03:b9cd` | 14 | 4 | 0 | 0 | 0 | 0 |
| 3 | `03:b9db` | 108 | 41 | **41** | 0 | 0 | 0 |
| 5 | `03:ba47` | 174 | 71 | 19 | **63** | 16 | 16 |
| 4 | `03:baf5` | 117 | 47 | 0 | 0 | **47** | 0 |
| 0 | `03:bbb9` | 82 | 31 | 0 | 0 | 0 | 0 |
| 1 | `03:bc0b` | 148 | 60 | 0 | 60 | 60 | 60 |

Bits 3, 4 and 5 each light up in exactly one session, which is what makes
those three attributions solid. Bit 1 runs in three sessions of four, so it is
not disaster-specific whatever it is. **Bit 0 has never run in any of the four
single-disaster sessions** — its 31 covered bytes all come from ordinary play.

The flood session produced **no distinguishing signal at all**: no arm
exclusive to it, and zero first-time-executed addresses in the whole ROM. So
either the flood is not dispatched through this ladder, or it did not fire
during that session. The data cannot tell those apart.

### The flood is not in the ladder

Confirmed behaviourally — a flood started, spread, destroyed a building and
receded — while the same session lit **no arm exclusive to it** and executed
**zero** first-time addresses. So the flood is not dispatched through `$0197`.
Its spread-then-recede behaviour is what a tile-level cellular process looks
like: a tile type that propagates on the per-tick map scan rather than an
event serviced once and cleared.

That is the answer to why six bits cannot cover every reported disaster. At
least one of them is not an event at all.

### What the flood *did* prove: the rewriter is shared demolition

Attributing every map-cell write per session, one writer appears in the flood
session and nowhere else:

| writer | flood | quake | monster |
|---|---|---|---|
| `03:B191` | 19,256 | — | 19,826 |
| `03:A53A` | 3,864 | — | 3,474 |
| **`03:A5C9`** | **10** | 0 | 0 |

`03:a5c9` is not a third arm. It is the `INY` inside the **write loop** of the
tile-`$0354` arm at `03:a59e` — the store itself is `03:a5c5`. Its absence
everywhere else means that in every other recording the arm was entered and
the pattern never fully matched, so the rewrite pass never ran.

The flood is therefore the only recorded event that has actually completed a
`$0354` structure demolition, which matches the building destroyed during it.
It also settles the earlier caution: the cell-pattern rewriter is **shared
demolition machinery invoked by whatever damages a structure**, not a
per-disaster routine. Declining to call `03:a553` "the tornado routine" was
right, and the reason is now evidenced rather than assumed.

### Where that leaves the ladder

Bits 0 (`03:bbb9`) and 1 (`03:bc0b`) remain unattributed, and the flood is out
of the running for both. Remaining candidates are fire, the Las Vegas UFO, and
the plane crash — the last of which cannot fire without a plane, so it may
never appear in a recording at all.


## Disaster attributions, settled in play

All six arms of the `$0197` ladder are now identified, by adding the triggers
to the F10 menu and firing each one in a real session:

| bit | handler | disaster |
|---|---|---|
| 0 | `03:bbb9` | **fire** |
| 1 | `03:bc0b` | **flood** |
| 2 | `03:b9cd` | **plane crash** |
| 3 | `03:b9db` | **tornado** |
| 4 | `03:baf5` | **earthquake** |
| 5 | `03:ba47` | **monster** |

### Three corrections this forces

**Bit 2 is the plane crash, not the nuclear meltdown, so `$0a8d` counts
airports.** The earlier reading was built on a single coincidence: `$0a8d` was
0 in the Boston state and the plants had been deleted there, so the guard
`LDA $0a8d ; BEQ` looked like a nuclear-plant check. That state had no airport
either. One save state agreeing with a hypothesis is not evidence for it when
another variable explains it equally well, and the guard is real — the arm
needs an airport to have anything to crash. `03:ac08 INC $0a8d` is where one
is built.

**The flood *is* in the ladder — bit 1.** An earlier section concluded it was
not, from a session where it fired but lit no arm exclusive to it. That
reasoning was sound but the premise was thin: bit 1 shows 60 of 148 bytes in
three sessions of four, which reads as "not disaster-specific" only if floods
are rare. They are not — the arm was running in most sessions because floods
kept happening. The spread-then-recede behaviour is still real, but it is what
the handler *does* after being dispatched, not evidence against dispatch.

**Bit 0 is fire, as the tile-property reading suggested.** `03:bbb9` requires
its target to carry bit 2 in the per-tile table at `$84eb`, and the
"flammable" guess was right. This is the one inference that survived, and it
was flagged as an inference at the time.

### What still holds

The cell-pattern rewriter at `03:a553` remains shared demolition machinery,
not per-disaster code — that conclusion came from writer attribution rather
than from guessing, and nothing here disturbs it.

## Entity types in the `$0ced` table

Every `JSR $c42a` in bank 03 is preceded by `LDA #imm` giving the type it
allocates. Cross-referencing the call sites against the disaster handlers'
address ranges names most of them:

| type | call site | inside | reading |
|---|---|---|---|
| `$00` | `03:c621`, `03:c66a`, `03:c67f` | — | |
| `$01` | `03:c4c9` | — | **never executed in any recording** |
| `$07` | `03:ba77` | monster arm `03:ba47` | monster |
| `$08` | `03:bdf3` | — | |
| `$0A` | `03:ba37` | tornado arm `03:b9db` | tornado |
| `$0B` | `03:bba0`, `03:bbf2` | `03:bb6a` and fire arm `03:bbb9` | shared effect |
| `$0C` | `03:bc74` | flood arm `03:bc0b` | flood |
| `$0E` | `03:bb12` | earthquake arm `03:baf5` | earthquake |
| `$13` | `03:c3f1` | — | |
| `$14` | `03:bd34` | — | |

### This casts doubt on the "monster step" label

`03:bb6a` was written up above as the monster's step. That is now suspect.
It allocates type `$0B`, and so does the **fire** arm at `03:bbf2` — while the
monster arm proper allocates `$07`. `03:bb6a` is also reached from `03:b939`
under a two-in-eight gate in the `$003e` path rather than from the ladder, and
it is near-identical in shape to the fire handler `03:bbb9` (random cell,
`$84eb` property test, stamp tile `$7F`), differing mainly in its strength
constant.

The likelier reading is that `$0B` is a shared damage/effect entity and
`03:bb6a` is a spread step that fire and the monster both drive, not the
monster specifically. It was attributed by difference against sessions, which
is weaker than attribution by containment — the session evidence only shows
that it ran when the monster did, and the monster and fire were not separated
at the time.

### Resolved: `03:bb6a` is not fire's spread step

The fire-alone session settles it, in favour of the original label. Fire was
triggered and ran — `03:bbb9` executed — while **`03:bb6a` did not execute at
all**, nor did the monster arm `03:ba47`:

| probe | fire-alone session | ever |
|---|---|---|
| `03:bbb9` fire arm | **yes** | yes |
| `03:bb6a` | **no** | yes |
| `03:ba47` monster arm | no | yes |

So `03:bb6a` is not driven by fire. It ran in the monster session and not in a
session where fire ran without a monster, which is now a genuine dissociation
rather than a co-occurrence. The shared type-`$0B` allocation is just both
paths using the same damage/effect entity, which is what `$0B` appearing at
two call sites meant all along.

The doubt was worth raising — the original attribution was by difference and
fire had never been isolated — but the label stands.

## The Las Vegas UFO

Captured with three save states bracketing an attack — before, during, and at
the popup — in a confirmed Las Vegas run (`$0040` = 6, year 2097).

**It is not a `$0197` disaster.** `$0197` reads `$0000` in all three states,
so the six-arm ladder is not involved, which is consistent with all six of its
arms already being accounted for.

**It is entity type `$14`.** Reading the `$0ced` table (10 slots of 6 bytes,
type in the first word) across the three states:

| state | slot 0 | slot 1 |
|---|---|---|
| before | `$0000` | free |
| during | `$0000` | free |
| **popup** | `$0000` | **`$0014`** |

and `03:bd34` — the only `LDA #$0014 ; JSR $c42a` in the bank — executed in
that session:

```
03:bd2b  STZ $0af1
03:bd2e  LDA #$0030 ; JSR $be04     ; post event $30
03:bd34  LDA #$0014 ; JSR $c42a     ; allocate entity type $14
03:bd3a  RTS
```

So the attack is an event post plus an entity allocation, the same shape the
monster and earthquake arms use, but reached from outside the ladder. No
`JSR`/`JSL` anywhere in the ROM targets `03:bd00-bd3a`, so `03:bd2b` is
entered by fall-through or a branch from earlier in the routine; that entry
point is not yet identified.

**Worth noting: the session executed zero first-time addresses.** The UFO path
was already covered by ordinary play in earlier recordings — it had simply
never been *attributed*. That is the same pattern as the earthquake session,
and it is the reason coverage growth stopped being a useful signal several
sessions ago: what is left is naming code that already runs, not finding code
that does not.

### `$0af1` is the "controls disabled" flag, and the spawn is reproducible

The attack replays deterministically from a save state taken before it: run
the "before" state forward and entity `$14` appears between frames 1800 and
3600, with no input at all. That makes it a fixture, not just an observation.

Watching `$0af1` across the replay shows the two phases. Write attribution
names the two instructions responsible — the flag is **set at `03:bce9`** and
**cleared at `03:bd2b`**:

```
03:bcda  LDA $bd4f,Y ; STA $0af7      ; parameter from a table at $bd4f
03:bce0  STZ $0af5
03:bce3  LDA #$8000
03:bce6  STA $0aef
03:bce9  STA $0af1                    ; approach: flag set
03:bcec  INY ; INY ; BRA $bcc5        ; loop
...
03:bd2b  STZ $0af1                    ; arrival: flag cleared
03:bd2e  LDA #$0030 ; JSR $be04       ; post event $30
03:bd34  LDA #$0014 ; JSR $c42a       ; spawn entity $14
```

`$0aef` gets `$8000` here too and is then rewritten every frame from
`00:c71f` — 1,111 writes in 1,200 frames — so it is an animation or countdown
running through the approach, not a flag.

> **Correction.** An earlier revision of this section quoted `03:bd1d`-`bd28`
> for the flag-set, an almost identical block (`LDA #$8000 ; STA $0aef ;
> STA $0af1 ; INY ; BRA`) forty bytes further on. **Those bytes never
> execute.** `dis_mx.py` had said so — none of that range carried the `*`
> execution mark while `03:bd2b` onward did — and the listing was read past
> without checking. The tool exists precisely to catch a plausible-looking
> decode of code that never runs, and it worked; the reader did not. Two
> near-duplicate blocks presumably serve two event kinds, only one of which
> is the UFO.

| frame | `$0af1` | entity slot 1 |
|---|---|---|
| start | 0 | free |
| 300-1800 | **`$8000`** | free |
| 3600+ | 0 | **`$14`** |

`$0af1` is held at `$8000` for the whole approach and cleared as the UFO
spawns — which matches the reported behaviour that **the controls go dead
just before the attack**, and explains `01:898a`'s `LDA $0af1 ; BNE $89e4`
in the reason-code path: nonzero, and the normal input handling is skipped.

`03:bd2b` is not a routine entry. It is the fall-out of a loop that branches
back to `03:bcf0` on the other path, and a PC-history trace catches it
arriving from bank 00 (`00:92b0`), i.e. after a call out and back. The loop's
own entry is still unidentified.

## `03:b16c` — the power-bit application pass

This is the routine behind `03:B191`, which dominates map-cell writes in every
recorded session (19,826 cells during a tornado replay, 19,256 during a
flood). It is not disaster code at all — it is how the power bitmap becomes
per-cell state:

```
03:b16c  TXA ; AND #$001e ; BNE $b17a     ; fetch a new bitmap word every 16 cells
03:b172  LDA $a598,Y ; XBA ; STA $00
03:b17a  LDA #$0000 ; STA $00023f
03:b181  LDA $0200,X ; AND #$7fff         ; map cell, bit 15 cleared
03:b187  ASL $00                          ; shift the bitmap word
03:b189  BCC $b18e ; ORA #$8000           ; carry set -> cell bit 15 = powered
03:b18e  STA $0200,X
03:b191  LDA $00023f ; BEQ $b19b ; ROR $00 ; BRA $b17a
03:b19b  INX ; INX
```

One bitmap bit per cell, walked 16 cells to a word, ORed into **bit 15** of
each map cell. That is exactly the bit the post-load power fix pokes —
`g_ram[0x10201 + i*2] |= 0x80` is `$7F0200 + i*2 + 1` bit 7, i.e. cell bit 15
— so the fix and this pass write the same thing, one from the host and one
from the guest. The bitmap source `$a598,Y` is the `$7FA598` array whose
absence from the SRAM save block caused the power dropout on load.

It also explains why this address swamps every write-attribution table: it
touches all 12,000 cells on every pass, so any per-disaster writer shows up
against a background two orders of magnitude larger.

### The UFO approach loop is reached from here

A PC-history trace catches `03:bcc5` — the loop whose `03:bce9` sets the
approach flag — arriving from this region (`03:b16c`-`03:b1a0`). So the Las
Vegas attack is driven off the per-cell simulation pass rather than a timer.
The exact edge is not established: the trace's last recorded PC before
`03:bcc5` is `03:b195`, whose `BEQ $b19b` does not lead there, so either the
history is not contiguous across the transition or the entry is via a path the
14-entry buffer did not capture. Worth a longer history before asserting it.


### `$01e7` — the View / extra-mode unlock bitfield

A bitfield gating two entries of the `$01fb` UI menu. The menu index is
computed at `01:AAEE` as `selector - 8`, giving 0-7, and two of those eight
are locked:

```
01:AAF9  CMP #$04 ; BNE $ab08
01:AAFE  LDA $01e7 ; AND #$01 ; BEQ $ab2d     ; index 4 needs bit 0
01:AB08  CMP #$07 ; BNE $ab15
01:AB0D  LDA $01e7 ; AND #$02 ; BEQ $ab2d     ; index 7 needs bit 1
```

`$ab2d` is the rejection path -- it jumps straight back to `01:AAD5`, so a
locked entry silently does nothing.

**Index 7 is the View mode** (the tilted Mode 7 map; cf. the `00:C0FB` view
fix this host patches). Identified from play, not statically.

Three ways the bit gets set, and the difference between them explains a
surprise:

| site | what |
|---|---|
| `01:BFF2` | `LDA $01e7 ; ORA #$02` when `$0397 == $0c` — **message/event ID 12** |
| `01:BFD1` | `ORA #$01` when `$0397 == $1f` (31) — unlocks index 4 |
| `03:C687` | `LDA #$0002 ; STA $01e7` — sets View outright, no condition |
| `03:CA3B` | `STA $01e7` from SRAM `$700064,X` — the unlock persists per saved city |

`$0397` is the pending message ID, queued at `03:BE04`
(`LDY $0395 ; BNE ; STA $0397 ; INC $0395`).

**So View is unlocked by the milestone *message*, not by the population
value.** In play it arrives at 50,000 people, but cheating the population
counter does not unlock it, because nothing queues message 12 — confirmed
empirically: the population was cheated past the threshold and View stayed
locked. Scenarios get it free instead, via `03:C687` setting it outright,
which is why View is available in every scenario from the start.

Measured across this repo's save states: `$01e7 = 0x0002` in savestates 3-6
(scenarios, View unlocked), `0x0000` in 0/1/2/7/8 (practice/free play),
`0x0001` in 9.

## FOUND: `$0c0d` is the per-scenario event countdown

The mechanism behind both scenario-scoped events, and it is one routine.

`03:ce8b` seeds `$0c0d` per scenario from the table at `03:cec9` (verified from
the code, not inferred):

| idx | scenario | year | `$0c0d` seed |
|---|---|---|---|
| 0 | San Francisco | 1906 | 10 |
| 1 | Bern | 1965 | 20 |
| 2 | Tokyo | 1961 | 5 |
| 3 | Detroit | 1972 | 3 |
| 4 | **Boston** | 2010 | **1** |
| 5 | Rio | 2047 | 258 |
| 6 | **Las Vegas** | 2096 | **384** |
| 7 | free play | 1991 | 10 |

The year column is the existing `03:ced9` table, and it pins the index mapping
independently.

### `03:b96f` dispatches on it, in two modes

**Mode 1 -- the countdown reaches 1: fire the scenario's signature disaster.**

```
03:b96f  LDA $0c0d ; CMP #$0001 ; BNE $b997
03:b977  LDY $0040
03:b97a  BNE  +           ; idx 0 San Francisco -> JSR $baf5  EARTHQUAKE
03:b981  CPY #$0002       ; idx 2 Tokyo         -> JSR $ba47  MONSTER
03:b98b  CPY #$0004       ; idx 4 Boston        -> JSR $bac1  <-- NOT a ladder arm
```

**Mode 2 -- otherwise, every 16th tick: the recurring events.**

```
03:b997  LDA $0c0d ; BEQ done
03:b99c  AND #$000f ; BNE +      ; only when the low nibble is 0
03:b9a1  LDY $0040
03:b9a4  CPY #$0005       ; idx 5 Rio        -> JSR $bc0b  FLOOD
03:b9ae  CPY #$0006       ; idx 6 Las Vegas  -> population gate -> JSR $bcb8  UFO
03:b9c4  LDY $0c0d ; BEQ done ; DEC $0c0d    ; tick
```

So **`03:bac1` and `03:bcb8` are a seventh and eighth handler**, outside the
six-arm `$0197` ladder entirely. That is why neither could ever be found in it.

### The meltdown: `03:bac1`

Boston seeds `$0c0d = 1`, so mode 1 fires on the **first tick** -- which is
exactly the reported behaviour, "triggered when you open up the scenario".

The handler is a full 120x100 map scan (`CMP #$0078` / `CMP #$0064`) for tile
value **`$027c`**, and on the first match it passes the cell coordinate through
`$0b85` and jumps to `03:bd61`.

`$027c` is one of the two self-powered tiles from `03:b0f8` -- the two power
plant types, where "which of the pair is coal and which is nuclear" was open.
A routine that hunts down `$027c` specifically and detonates it settles it:
**`$027c` is the nuclear plant, `$028c` the coal plant.**

### The UFO: `03:bcb8`

Las Vegas seeds `$0c0d = 384`, so mode 2 fires every 16 ticks for 24 events,
gated on population (`$0ba5`/`$0ba7` vs `$1_4c08`). The handler walks a 9-entry
waypoint path from `03:bd3b`/`03:bd4f`, terminated by `$00ff`, and sets
`$0af1 = $8000` -- the "controls disabled" flag already documented below.

### Confirmed by execution

Save states 3-6 are Las Vegas (`$0040 = 6`, year 2097) sitting at
`$0c0d = 304` -- counted down 80 from 384, and **divisible by 16**, i.e. exactly
on the firing condition. Running `savestate_3` for 4,000 frames and checking the
coverage bitmap:

| site | | |
|---|---|---|
| `03:b96f` | countdown read | EXECUTED |
| `03:b99c` | `AND #$000f` every-16 gate | EXECUTED |
| `03:b9ae` | `CPY #$0006` Las Vegas arm | EXECUTED |
| `03:b9b3` | population gate | EXECUTED |
| `03:b9c1` | `JSR $bcb8` | EXECUTED |
| `03:bcb8` | UFO handler | EXECUTED |
| `03:b9c9` | `DEC $0c0d` | EXECUTED |
| `03:b990` | `JSR $bac1` meltdown call | **not executed** |
| `03:bac1` | meltdown handler | **not executed** |

The negative half is the control: from a Las Vegas state the mechanism fires the
Las Vegas arm and not the Boston one.

### The gate: `$003e == 3`

`03:b96f` is not fallen into — `03:b969` is the RNG threshold **table**
(`12c0`/`0960`/`04b0` = 4800/2400/1200, indexed by difficulty `$0b57`). The
dispatcher is a separate routine with exactly one caller:

```
03:b858  LDA $0425 ; AND #$0001 ; BEQ +     ; No-Disasters cheat -> skip all
03:b863  LDA $003e ; CMP #$0003 ; BNE +
03:b86b  JSR $b96f                          ; <- only when $003e == 3
```

`$003e == 3` is scenario mode (the win-mark setter at `03:e2ee` is gated on the
same thing). This is why save states 0/1/2/7/8/9 never reach the dispatcher at
all: they are free-play cities (years 1900-1904, `$0c0d = 0`), not scenarios.

### The meltdown, confirmed by execution

From `savestate_3` (Las Vegas, so `$003e == 3` already holds), setting
`$0040 = 4` and `$0c0d = 1` runs the whole chain:

| site | | |
|---|---|---|
| `03:b86b` | `JSR $b96f` past the `$3e` gate | EXECUTED |
| `03:b98b` | `CPY #$0004` Boston arm | EXECUTED |
| `03:b990` | `JSR $bac1` | EXECUTED |
| `03:bac1` | meltdown handler | EXECUTED |
| `03:bad1` | `CMP #$027c` nuclear tile | EXECUTED |
| `03:badd` | `JMP $bd61` **detonate** | EXECUTED |

Reaching `03:badd` means the scan **found** a `$027c` tile and detonated it.

This confirms the dispatch and the handler. It does not confirm that the Boston
*map* is what gets destroyed, since the scan ran over the Las Vegas map — but
Boston seeding `$0c0d = 1` means the same path runs there on the first tick.

### The UFO population gate

`03:b9b3` is a 32-bit compare of `($0ba7:$0ba5)` against `$0001_4c08`, so the
UFO needs a population of at least **84,488**. Measured: on a small free-play
city the Las Vegas arm is reached and the gate rejects it before `JSR $bcb8`.

### On a non-Las-Vegas map: damage, but no UFO

Reported from play: triggering the UFO on a practice or ordinary map does
damage but shows no UFO. Measured, and it is **not** a code-path difference.

| | real Las Vegas | forced on free play |
|---|---|---|
| approach loop `03:bcc5` | YES | YES |
| waypoints exhausted `03:bcd5` | YES | YES |
| arrival `03:bd2b` | YES | YES |
| event post `03:bd2e` | YES | YES |
| `LDA #$0014 ; JSR $c42a` | YES | YES |
| renderer `00:c402`-`c752`, 15 sites | YES | **YES, all 15** |

The whole sequence runs, including every site in the bank-00 code that reads
`$0aef`/`$0af1`/`$0af9`. So the logic and the drawing code both execute; what
differs is the data they draw with. The leading explanation is that the UFO
sprite tiles are not in VRAM outside its own scenario — **not established.**

Ruled out: a scenario-keyed graphics load. The only read of `$0040` outside
bank 03 is `08:a62d`, and bank 08 is compressed data — the surrounding
disassembly is `MVN`/`COP`/`WAI` nonsense, so that is a byte coincidence, not
an instruction.

> **Correction: `03:c42a` is not an entity spawner.** It writes the value
> passed in `A` to `$0ced,X`, then `$0b53` (year) and `$0b55` (month) to
> `$0cef,X`/`$0cf1,X` — a **dated message log**, ten six-byte slots, shifted
> down when full. So `LDA #$0014 ; JSR $c42a` posts a dated "UFO" news entry;
> it does not create a sprite. Earlier notes here read it as "spawn entity
> $14" / "allocate entity type $0B", and the moving-object table is a
> different range entirely (`$02cb`-`$0376`). The UFO's visible form comes
> from the approach state (`$0aef`/`$0af5`/`$0af7`/`$0af9`), not from `$c42a`.

**Next step that would settle it:** compare VRAM/OAM during the approach
between the two runs. If OAM carries the sprite but its tiles are absent, the
graphics reading is confirmed.

### Skipping the UFO population gate

The `UFO` menu row lifts the gate for the duration of the event by NOPping the
branch itself (`03:b9bf`, `90 03` -> `EA EA`), then putting it back.

Patching the **code** rather than writing a fake population is the conservative
choice: `$0ba5`/`$0ba7` are live simulation state that taxes, milestones and the
win check all read, so faking them even briefly would change the game in ways
nothing here could bound. Two bytes of branch affect exactly this decision.

> **`cart_init()` copies the ROM.** `cart->rom = malloc(); memcpy(...)`, so the
> buffer `read_file()` returned is *not* what executes. The boot-time patches
> work only because they run before the cart is built. A patch applied later
> must go to `cart->rom` or it silently does nothing — which is exactly what the
> first version of this did: the gate reported "lifted" and the UFO still did
> not appear.

Note the forced state lasts as long as the event does. The UFO handler spins at
`03:bcc5` for the whole approach, so `DEC $0c0d` — and with it the restore —
comes ~2,800 frames later. Measured end to end: armed at 65873, restored at
68647, with `$3e`, `$0040` and the gate all put back.

### Triggering both from the F10 menu

`MELTDOWN` and `UFO` rows, and the headless twin
`SC_SCENARIO_EVENT=<meltdown|ufo>@<frame>`.

Both set three words together — `$003e = 3`, `$0040` = 4 or 6, and `$0c0d` = 1
or 16 — then **restore `$003e`/`$0040` on the ROM's own first `DEC $0c0d`**.
Two of those words identify the city, so leaving them changed would tell the
game it is in a different scenario and corrupt the win check and the next save.
Restoring on the ROM's decrement is self-timing: simulation ticks are ~160
frames apart and vary with game speed, so a fixed frame delay would be guesswork.

Not a freeze — the values are set once and the ROM consumes them, so execution
stays on paths the game really takes.

## Meltdown and UFO are scenario-driven, not `$0197` bits

Reported from play, and it fits everything measured:

> "Meltdown is triggered when you open up the scenario, and it is more likely
> when you started a difficult game."

So the nuclear meltdown is **not** one of the six `$0197` arms. That resolves
the long-running confusion in this file, where bit 2 was first read as the
meltdown and later corrected to the plane crash — the meltdown was never in
the ladder to be found.

The same is true of the UFO, already established as entity type `$14` and
outside the `$0197` ladder. Both are scenario-scoped events:

| event | scope | mechanism |
|---|---|---|
| six `$0197` arms | any city | disaster-selection page sets a bit, `03:b8ae` services it |
| nuclear meltdown | Boston scenario | set up when the scenario is opened |
| UFO | Las Vegas scenario | entity type `$14` |

The difficulty half of the report is already quantified here: `$0b57` indexes
`03:b969`, and each step **doubles** the per-tick disaster chance (1 in 4801 /
2401 / 1201). Whether that same word also gates the scenario-scoped events, or
only the ladder's spontaneous firing, is not established — the RNG draw at
`03:b91e` is on the ladder path, so on present evidence it scales the six, and
the report's "more likely on hard" may be about those rather than the meltdown
specifically.

### Confirmed: each `$0197` bit drives its own arm

`SC_DISASTER=<bit>@<frame>` (src/main.c) sets one bit headlessly — the same
thing the F10 menu and the game's own disaster page do. One run per bit from
`savestate_9`, diffed against a no-disaster baseline, counting newly executed
addresses inside each handler body:

| bit set | new addresses in its own handler |
|---|---|
| 0 | 33 |
| 1 | 65 |
| 2 | 4 |
| 3 | 41 |
| 4 | 46 |
| 5 | 46 |

Every bit lights its own arm, and bits 3 and 5 reproduce the tornado and
monster attributions that came from hand-played sessions — so the method is
validated against known answers before being trusted on the unknown ones.

Bit 2's mere 4 addresses are the `$0a8d` guard bailing out: `savestate_9` has
no airport, so the plane crash has nothing to crash.

## Putting the meltdown and UFO on the game's own disaster page

`SC_DISASTER_MENU8=1`. The page is `01:aa39` (screen mode `$01df == 2`), which
walks `$0197` as a checkbox list:

```
01:aa3e  ASL A ; ASL A     ; 2 shifts -> only bits 5..0 reach the walker
01:aa45  LDY #$0005        ; 6 rows
01:aa77  LDA $01a95c,X     ; bit-mask table
```

Two things make this cheap, and both were surprises:

- **The mask table already runs to `$0200`.** Bits 6 and 7 have masks sitting
  at `01:a968`/`01:a96a`, unused.
- **The input path already accepts eight rows.** `01:aa6b CMP #$0008` then
  `SBC #$0008` with only a `BMI` bail, so indices 0-7 pass. Only the *render*
  side is capped at six.

So two byte patches do it: `ASL A ; ASL A` -> `NOP NOP` so all eight bits reach
the walker, and `LDY #$0005` -> `LDY #$0007`.

The new bits are serviced **host-side**, not by extending `03:b8ae`. That
ladder is a fixed chain ending in `PLD`/`RTS` at `03:b914` with no room for two
more arms — and the meltdown and UFO are not ladder disasters anyway, they are
the `$0c0d` scenario events. So the ROM patch only has to make the bits
*settable*; the host reads them and arms the existing verified trigger.

Verified: `SC_DISASTER=6@<frame>` arms the meltdown and `7` the UFO, each
through the full chain and each restoring afterwards. `--qualify` is
byte-identical with and without the patch, so it is inert until a bit is set.

### Two things about it are unverified

**The new rows have no labels.** Row text comes from the page-setup dispatch
(`01:aabf JSR ($9d1a,X)`), not from the checkbox renderer at `01:a918`/`a93a`,
which only writes the box tile to `$7e2063 + row*16`. Rows 6 and 7 will draw a
checkbox with nothing beside it until that table is found and extended.

**Where rows 6 and 7 land is unknown.** They write 16 and 32 bytes past the
last existing row; whether that is inside the menu box or on top of whatever is
below it has not been checked. This is why the patch is opt-in.

## The `$0197` ladder, fully attributed

Confirmed by observation: each bit was triggered from the F10 menu on a live
city and the resulting disaster identified on screen.

| bit | handler | disaster |
|---|---|---|
| 0 | `03:bbb9` | **fire** |
| 1 | `03:bc0b` | **flood** |
| 2 | `03:b9cd` | **plane crash** |
| 3 | `03:b9db` | **tornado** |
| 4 | `03:baf5` | **earthquake** |
| 5 | `03:ba47` | **monster** |

Bits 2-5 were fired and watched in one session (frames 72130, 72353, 73004,
73818); bits 3 and 5 reproduced the tornado and monster already attributed from
hand-played single-disaster runs, which is the check that the method is sound.
Bits 0 and 1 carry the same names on the reporter's knowledge of the game
rather than from that particular session.

This closes the attribution that "roughly 60% of the six handlers' code has
never executed" was blocking. It also retires the recurring temptation to look
for the meltdown here: the six are fire, flood, plane crash, tornado,
earthquake and monster, and **the meltdown is not among them** — it is
scenario-scoped, as is the UFO.

### Reconciling "the flood is not in the ladder"

The section above concluded, from a flood that started, spread and receded
while lighting no exclusive arm and executing zero first-time addresses, that
the flood is not dispatched through `$0197`. Bit 1 being the flood does not
overturn that; the two fit together:

- **Bit 1 starts a flood.** The arm at `03:bc0b` is the *initiator* — it seeds
  the event once and the ladder clears the bit.
- **The flood's behaviour is cellular.** Spread and recession happen on the
  per-tick map scan, not through the ladder, which is why a *naturally
  occurring* flood needs no arm and lights none.

So the earlier session watched a flood that was already running, and correctly
observed that its ongoing behaviour is not ladder-driven. The initiator and the
process are separate, and only the initiator is a `$0197` bit. Worth keeping
both readings: "not in the ladder" is right about the spread and wrong only if
read as "no bit starts it".

## `03:B92E` — the spontaneous disaster selector (never executed)

After the difficulty-scaled RNG draw passes (`03:B91E`, 1 in 4801/2401/1201),
a second draw picks *which* disaster. The whole selector at `03:B93E-B966` has
**never executed in any recording**, and it decodes at m=0 — a width-blind read
here produces `BRK` garbage, which is presumably why it stayed dark.

```
03:b92e  JSR $907e ; AND #$07        ; random 0..7
03:b934  CMP #$02 ; BCS $b93e
03:b939  JSR $bb6a                   ; draws 0-1
03:b93e  CMP #$0004 ; BCS $b948
03:b943  JSR $bc0b                   ; draws 2-3   flood
03:b948  CMP #$0005 ; BNE $b952
03:b94d  JSR $b9db                   ; draw  5     tornado
03:b952  CMP #$0006 ; BNE $b95c
03:b957  JSR $baf5                   ; draw  6     earthquake
03:b95c  LDA $0c07 ; CMP #$0050 ; BCC $b967
03:b964  JSR $ba47                   ; draws 4,7   monster, gated
```

| draw | disaster | share |
|---|---|---|
| 0-1 | `03:bb6a` | 2/8 |
| 2-3 | flood | 2/8 |
| 5 | tornado | 1/8 |
| 6 | earthquake | 1/8 |
| 4, 7 | monster, **if `$0c07` >= `$50`** | 2/8 |

Note draw 4 falls through both equality tests and lands on the monster gate, so
the monster gets two draws rather than one.

**`$0c07 >= 80` gates the monster.** That is a threshold on city state the
monster needs before it can appear — the shape of a population or size gate,
though which is not established here.

`03:bb6a` taking draws 0-1 is the only arm of this selector that has ever run,
which fits: an earlier session established it is *not* fire's spread step. Fire
(`03:bbb9`) and the plane crash (`03:b9cd`) are the two ladder handlers this
selector never calls directly, so `03:bb6a` choosing between them is the
obvious hypothesis — and explicitly only a hypothesis.

### What this says about the meltdown

The meltdown is not here either. The spontaneous path can raise flood,
tornado, earthquake, monster and whatever `03:bb6a` picks — six ladder
handlers, no seventh. Combined with the ladder itself being fully attributed,
the meltdown is not reachable by setting any `$0197` bit or by any random
draw, which is consistent with the reported behaviour that it comes with the
scenario.

So a menu trigger for it cannot work the way the six do. It needs whatever the
Boston scenario sets up at load time, and that is the next thing to find —
`03:ce2e` (scenario map loader) and `03:ddb6` (scenario select) are the places
to look.
