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

`03:ce70` is **data, not code**: three parallel byte arrays, 8 entries each,
column-major — low bytes at `03:ce70`, high bytes at `03:ce79`, bank bytes at
`03:ce82`. This struct-of-arrays layout is why ordinary pointer scans never
found them.

| idx | scenario | map pointer |
|---|---|---|
| 0 | San Francisco | `0d:9f23` |
| 1 | Bern | `0c:e30b` |
| 2 | Tokyo | `0c:8f27` |
| 3 | Detroit | `0c:c5a2` |
| 4 | Boston | `0c:a8e8` |
| 5 | Rio | `0d:816e` |
| 6 | Las Vegas | `0d:b987` |
| 7 | free play | `0d:cb15` |

Indices 6 and 7 have real map data, so the two scenarios with no slot in the
selection grid are complete, not stubs.

Cross-check: the third-party "Sim City - Sylt" hack patches exactly file offsets
`0x1ce75` / `0x1ce7e` / `0x1ce87`, which are index 5 (Rio) in each of the three
arrays — confirming both the layout and the index.

Real code resumes at `03:ce8b`: `REP #$30 ; LDA $0040 ; ASL ; TAY ;
LDA $cec9,Y` — another per-scenario table at `$03cec9`, purpose not yet
identified.

## Open: the ROM maps are compressed, format unknown

Consecutive pointers sit ~7-8KB apart while the uncompressed map is 24000
bytes, so the ROM data is compressed. Two hypotheses have been tested against
`0d:816e` and **both failed**:

- `tools/extract_graphics.py`'s `nintendo_decompress()` consumed 2 bytes then
  hit malformed data — no valid LC_LZ5 header at that offset.
- Read as raw 16-bit cells, only 21.6% of values fall in the 10-bit index range
  and there are 5156 distinct values — definitively not a raw cell array.

Next step is to stop guessing and instrument the load path: trace the
decompressor at `00:90dd` during a scenario load and capture its source address
and output length. Verification is easy because correct output is known
exactly — 24000 bytes of 10-bit indices.
