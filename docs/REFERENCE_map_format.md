# Map data format

Source: **SNES SimCity Map Viewer**, a BizHawk Lua script by **Brad Smith**
(2022-08-30), <https://rainwarrior.ca> — support the author at
<https://www.patreon.com/rainwarrior>.

The script itself is **not redistributed here**: it carries no stated licence,
so it is kept locally (gitignored) rather than committed. Everything this
project actually needs from it is recorded below, verified against our own ROM.
Fetch the original from the author if you want to run it.

## Live map in WRAM

| what | address | notes |
|---|---|---|
| map data | `$7F0200` | BizHawk WRAM offset `0x010200` |
| palette | `$7E2440` | BizHawk WRAM offset `0x002440` |
| tile table | `02:d6a9` | file `0x0156A9` (USA). Verified: byte there is `a5` |
| upper/overlay tile table | file `0x014f2d` | `TILE_ADDR - 0x77C` |

Dimensions are **120 × 100 = 12000 cells**, **2 bytes per cell** = 24000 bytes.

Per cell:

```
v  = read_u16_le($7F0200 + i*2) & 0x03FF   ; 10-bit tile index (1024 types)
tp = read_u16_le(TILE_ADDR + v*2)          ; index -> background tile
tu = read_u16_le(TILU_ADDR + v*2)          ; index -> overlay tile
```

Overlay value `0x300` means "empty" and is skipped. The overlay tile for a cell
is drawn offset by −1,−1, i.e. it overlaps the cell up and left of itself.

Tile CHR is read from VRAM as standard 4bpp SNES: `tc & 0x03FF` selects the
tile, `(tc >> 10) & 7` selects the palette. The palette itself is BGR555 read
from the WRAM buffer above — which independently corroborates the palette
buffer found at `01:dd76` while mapping the message system (see
`INVESTIGATION_dpad.md` / task notes), reached from a completely different
direction.

## Scenario maps in ROM

Found separately (see the scenario tasks), and *not* from the Lua script — the
script only reads the live WRAM map.

`03:ce70` is **data, not code**: three parallel byte arrays, **9 entries each**,
column-major — low bytes at `03:ce70`, high bytes at `03:ce79`, bank bytes at
`03:ce82`. This struct-of-arrays layout is why ordinary pointer scans never
found them.

| idx | scenario | map pointer | file | 3×3 stamps |
|---|---|---|---|---|
| 0 | San Francisco | `0d:9f23` | `069f23` | 224 |
| 1 | Bern | `0c:e30b` | `06630b` | 211 |
| 2 | Tokyo | `0c:8f27` | `060f27` | 263 |
| 3 | Detroit | `0c:c5a2` | `0645a2` | 230 |
| 4 | Boston | `0c:a8e8` | `0628e8` | 249 |
| 5 | Rio | `0d:816e` | `06816e` | 331 |
| 6 | Las Vegas | `0d:b987` | `06b987` | 219 |
| 7 | free play | `0d:cb15` | `06cb15` | 0 |
| 8 | tutorial | `0d:d131` | `06d131` | 0 |

**The table has nine entries, not eight.** The arrays run `03:ce70`–`03:ce8a`
(9 × 3 bytes) and real code only resumes at `03:ce8b`, so the ninth entry was
being read as padding. Three independent things say it is real:

- the bank column is `0d 0c 0c 0c 0c 0d 0d 0d 0d` — nine bytes, and the three
  arrays together run out exactly at `03:ce8b`, where `REP #$30` starts real
  code, with no room left over;
- `0d:d131` is precisely where index 7's compressed stream ends, closing the
  one inter-pointer gap that otherwise did not add up;
- it decodes cleanly to a full 24000-byte map.

An earlier reading took the bank column as eight entries and so stopped one
short. Note the per-scenario tables at `03:cec9` genuinely *are* eight entries
(see below), so eight is the right count there and the wrong one here.

All nine decode. Index 6 (Las Vegas) is a desert grid city with a diagonal
across it, so the hidden scenario is complete, not a stub.

Indices 7 and 8 differ in kind from 0–6, not just in content: they use **zero**
3×3 stamps (stage 4 below) and consume exactly 12000 entries, i.e. they are
pure terrain with no buildings placed, where every real scenario carries 200+
building stamps. They are maps you build on rather than cities you inherit,
which is exactly why neither places a single building.

Index 7 is free play, and its terrain draws **Mario's face** as an island —
worth knowing before assuming a decode has gone wrong. Index 8 is the tutorial
map (a lake with islands).

A parallel set of per-scenario tables settles index 7's status independently of
the artwork. `03:ce8b` seeds five values from `$0040`:

| table | → | contents |
|---|---|---|
| `$03cec9` | `$0c0d` | `000a 0014 0005 0003 0001 0102 0180 000a` |
| `$03ced9` | `$0b53` | **year**: 1906, 1965, 1961, 1972, 2010, 2047, 2096, 1991 |
| `$03cee9` | `$0deb`/`$0ca5` | starting city class |
| `$03cef9` | `$0ba5` | starting population, low word |
| `$03cf09` | `$0ba7` | starting population, high word |

The years are decisive on the ordering — 1906 San Francisco, 1965 Bern, 1961
Tokyo, 1972 Detroit, 2010 Boston, 2047 Rio are the six canonical scenarios in
exactly this sequence, then 2096 for Las Vegas and 1991 for index 7. These
tables hold **eight** entries, so index 7 is an ordinary selectable mode with a
starting year while index 8 (tutorial) is absent from them entirely and must be
special-cased elsewhere.

`tools/extract_maps.py` decodes all nine to `extracted_assets/maps/`, as both a
raw 24000-byte `.bin` in the live `$7F0200` layout and a false-colour `.png`
preview.

Cross-check: the third-party "Sim City - Sylt" hack patches exactly file offsets
`0x1ce75` / `0x1ce7e` / `0x1ce87`, which are index 5 (Rio) in each of the three
arrays — confirming both the layout and the index.

Real code resumes at `03:ce8b`: `REP #$30 ; LDA $0040 ; ASL ; TAY ;
LDA $cec9,Y` — another per-scenario table at `$03cec9`, purpose not yet
identified.

## Solved: the compressed map format

The maps are LC_LZ5 after all, followed by three more unpacking stages. The
earlier "not LC_LZ5 at `0d:816e`" result was wrong — `nintendo_decompress()`
decodes that stream fine at the correct file offset (`0x06816e`, i.e.
`bank × 0x8000 + (addr − 0x8000)`), so that hypothesis had been rejected on a
bad offset rather than on the format.

The loader is `03:ce2e` and the unpacker `03:d15f`. Decoded from the ROM (both
routines need hand-decoding — the disassembler misaligns across `SEP`/`REP`,
so e.g. `c9 ff` at `00:9102` is the 8-bit `CMP #$ff` terminator test, not a
16-bit compare):

**Stage 0 — pointer.** `03:ce3c` reads the scenario index from `$0040` and
pulls a 24-bit pointer out of the three arrays above into `$09`/`$0a`/`$0b`,
sets `$0e = 0`, and dispatches.

**Stage 1 — LC_LZ5.** `COP #$00` with `A = 8` is the syscall for the shared
decompressor at `00:90dd`. Source `$0b:$0009`, destination **`$7E8000 + $000e`**
(not `$7E0000`; `X` is 16-bit, so the window reaches `$7F7FFF`). On the source
pointer wrapping past `$ffff`, `00:926d` sets `Y = $8000` and increments the
*data-bank register*, so the stream is contiguous in file-offset terms and a
compressed length must be measured in file offsets — subtracting the 24-bit
addresses over-counts each bank crossing by `$8000`. Produces 2.8–13KB of
16-bit words.

**Stage 2 — word LZ.** `03:d16c` copies words from `$7E8000` to `$7F0200`:
`$ffff` ends the stream, bit 14 marks a back-reference (distance =
`word & $03ff` bytes, count = `(word >> 10) & $0f` words), anything else is a
literal. Every shipped map is pure literals here — LC_LZ5 has already done that
job — but the stage exists in the ROM.

**Stage 3 — run length.** `03:d1c0` expands back into `$7E8000`: each word
carries a repeat count in bits 13–10 and a payload of `word & $83ff`, emitted
count+1 times.

**Stage 4 — stamps.** `03:d1fb` zero-fills the map (`MVN $7f,$7f` with
`A = $5dbf`, i.e. **exactly 24000 bytes** — this is where the map's size is
pinned down in the ROM itself), then `03:d210` walks all 12000 cells in order.
A cell that is already non-zero is skipped *without consuming an entry*;
otherwise the next entry is taken and bit 15 selects the shape — clear writes a
single cell, set writes a 3×3 block of nine *consecutive* indices
(`v, v+1, … v+8`) at row stride `$f0`/`$1e0` = 240 bytes, which is what fixes
the map width at 120 cells.

Stage 4 is why stage 3's output is short of 12000 words for indices 0–6: a 3×3
stamp covers nine cells but costs one entry. The arithmetic closes exactly for
every map — `(12000 − entries)` is divisible by 8 in all nine cases — and each
compressed stream's length equals the gap to the next pointer, so the streams
are packed back to back with nothing unaccounted for.

### How this was verified

Not by inspection. `SC_DECOMP_TRACE=1` (added to `src/main.c`) logs every call
to `00:90dd` with its source pointer, destination, and true output extent
measured off the bus. Running it from a cold boot showed the game load a fixed
64KB blob into `$7F` in two halves at frames 109 and 138 — `0d:d77c` → `$7E8000`
(32768 bytes out), then `MVN $7f,$7e` at `05:9304` copying it to `$7F0000`.

That gave a free oracle: a known ROM input with a known 32768-byte output. A
Python reimplementation of `00:90dd` reproduces it **byte-exact**, which is what
makes the rest of the chain trustworthy. `SC_MAP_WRITE_TRACE=1` (writes into
`$7F0200`, with a distinct-PC histogram) is what found the `MVN`, and stays
useful for anything that touches the live map.

Independent visual confirmation: the decoded maps render as recognisably San
Francisco (peninsula and bay), Tokyo (bay, with the Imperial Palace grounds as a
distinct block), Las Vegas (desert grid with the diagonal across it), free
play's Mario-face island, and the tutorial map's lake and islands.

### Confirmed against the live game

Closed with save states captured by hand at the scenario-select screen, on a
freshly loaded San Francisco, and on a freshly loaded free play, then diffing
each live `$7F0200` against this decode:

| map | result |
|---|---|
| free play | **24000 / 24000 bytes identical — 100.00%** |
| San Francisco | 11910/12000 cells exact; 10-bit tile index agrees on 11974/12000 (99.78%) |

Free play matching to the byte is the strong result: it had just been loaded and
the simulation had not yet altered it, so it is a clean comparison and the decode
reproduces the game's own buffer exactly.

San Francisco's residual is fully accounted for and is not decode error:

- **64 cells differ only in bit 14** (`$4000`). That is a live simulation status
  flag on the cell, not part of the stored map — consistent with the format
  above, which takes the tile index as `value & $03FF` and leaves the upper bits
  to the game.
- **26 cells differ in the tile index itself**, i.e. the simulation had already
  mutated them. Running the same state on for 90 more frames raises the count
  (90 → 299 differing cells), which is what ongoing simulation looks like and
  what a decode error would *not* do.

### Free play uses the stored map, not the generator

Also settled by that capture: free play (index 7) loads `0d:cb15` through the
ordinary path — its live map matched this decode byte for byte, with the map
seed `$0b27`-`$0b29` all zero. So the procedural generator at `03:d840` (a real
PRNG-driven terrain generator, see `docs/ROM_MAP.md`) is *not* what produces the
free-play map, despite both existing. Which mode does drive the generator is not
established here.
