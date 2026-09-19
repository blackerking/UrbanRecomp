#!/usr/bin/env python3
"""Decode the eight scenario maps out of the ROM.

Run from the repo root with your own US ROM there (any file name; it is
found by its contents, see tools/find_rom.py):

    python tools/extract_maps.py

Writes `extracted_assets/maps/<scenario>.bin` (24000 bytes: 120x100 cells,
one little-endian 16-bit tile index each, exactly the layout the game keeps
live at $7F0200) plus a false-colour `<scenario>.png` preview per scenario.
`extracted_assets/` is gitignored -- it is derived from the copyrighted ROM.

--------------------------------------------------------------------------
The format
--------------------------------------------------------------------------
Each scenario's map is a four-stage pipeline, all of it decoded from the ROM
itself (`03:ce2e` is the loader, `03:d15f` the unpacker) and verified against
the running game -- see docs/REFERENCE_map_format.md.

  0. Pointer.  `03:ce2e` reads the scenario index from `$0040` and pulls a
     24-bit source pointer out of three parallel byte arrays at `03:ce70`
     (low), `03:ce79` (high), `03:ce82` (bank).

  1. LC_LZ5.   `COP #$00` with A=8 dispatches to the shared decompressor at
     `00:90dd`, source `$0b:$0009`, destination `$7E8000 + $000e`. Produces
     ~4-13KB of 16-bit words.

  2. Word LZ.  `03:d16c` copies words from `$7E8000` to the map buffer at
     `$7F0200`; `$FFFF` ends the stream, bit14 marks a back-reference
     (distance = word & $03FF bytes, count = (word >> 10) & $0F words), and
     anything else is a literal. In practice every shipped scenario map is
     pure literals here -- LC_LZ5 has already done that job -- but the stage
     is implemented because the ROM implements it.

  3. Run-length. `03:d1c0` expands back into `$7E8000`: each word carries a
     repeat count in bits 13-10 and a payload of `word & $83FF`, emitted
     count+1 times.

  4. Stamps.   `03:d1fb` zero-fills the 24000-byte map (`MVN $7f,$7f` with
     A=$5dbf, i.e. 24000 bytes -- this is where the map's exact size is
     pinned down), then `03:d210` walks all 12000 cells in order. A cell
     that is already non-zero is skipped without consuming an entry;
     otherwise the next entry is taken, and bit15 selects the shape: clear
     writes a single cell, set writes a 3x3 block of nine *consecutive*
     indices (value, value+1, ... value+8) at row stride 240 bytes -- which
     is what fixes the map width at 120 cells.

Stage 4 is why the stage-3 output is short of 12000 words for seven of the
eight scenarios: each 3x3 stamp covers nine cells but costs one entry. The
arithmetic closes exactly -- (12000 - entries) is divisible by 8 for every
scenario, and free play, which uses no stamps at all, lands on 12000 on the
nose.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_graphics import nintendo_decompress  # noqa: E402

MAP_W, MAP_H = 120, 100
MAP_CELLS = MAP_W * MAP_H          # 12000
MAP_BYTES = MAP_CELLS * 2          # 24000, matches the MVN count at 03:d1fb
ROW_STRIDE = MAP_W * 2             # 240, matches the +$f0/+$1e0 stamp offsets

# 03:ce70 / 03:ce79 / 03:ce82, column-major -- see docs/REFERENCE_map_format.md
SCENARIOS = [
    ('san_francisco', 0x0D, 0x9F23),
    ('bern',          0x0C, 0xE30B),
    ('tokyo',         0x0C, 0x8F27),
    ('detroit',       0x0C, 0xC5A2),
    ('boston',        0x0C, 0xA8E8),
    ('rio',           0x0D, 0x816E),
    ('las_vegas',     0x0D, 0xB987),
    ('free_play',     0x0D, 0xCB15),
    ('tutorial',      0x0D, 0xD131),
]


def file_offset(bank, addr):
    """LoROM: bank N's $8000-$ffff window is file offset N*0x8000."""
    return bank * 0x8000 + (addr - 0x8000)


def stage_word_lz(blob):
    """Stage 2 -- 03:d16c."""
    out = bytearray()
    pos = 0
    while True:
        if pos + 2 > len(blob):
            raise ValueError('word-LZ stream ran off the end at %d' % pos)
        w = blob[pos] | (blob[pos + 1] << 8)
        pos += 2
        if w == 0xFFFF:
            return bytes(out), pos
        if w & 0x4000:
            src = len(out) - (w & 0x03FF)
            if src < 0:
                raise ValueError('word-LZ back-reference before start at %d' % pos)
            for _ in range((w >> 10) & 0x0F):
                out += out[src:src + 2]
                src += 2
        else:
            out.append(blob[pos - 2])
            out.append(blob[pos - 1])


def stage_rle(mid):
    """Stage 3 -- 03:d1c0."""
    out = bytearray()
    for i in range(0, len(mid) - 1, 2):
        w = mid[i] | (mid[i + 1] << 8)
        if w == 0xFFFF:
            break
        v = w & 0x83FF
        out += struct.pack('<H', v) * (((w >> 10) & 0x0F) + 1)
    return bytes(out)


def stage_stamps(entries):
    """Stage 4 -- 03:d1fb (zero-fill) and 03:d210 (walk/stamp)."""
    cells = [0] * MAP_CELLS
    src = 0
    stamps = 0
    for cell in range(MAP_CELLS):
        if cells[cell]:
            continue                       # 03:d214's BNE -- already stamped
        if src + 2 > len(entries):
            raise ValueError('ran out of entries at cell %d' % cell)
        v = entries[src] | (entries[src + 1] << 8)
        src += 2
        if v & 0x8000:
            base = v & 0x7FFF
            for row in range(3):
                for col in range(3):
                    cells[cell + row * MAP_W + col] = base + row * 3 + col
            stamps += 1
        else:
            cells[cell] = v
    return cells, src, stamps


def decode_map(rom, bank, addr):
    blob, lz_end = nintendo_decompress(rom, file_offset(bank, addr))
    mid, _ = stage_word_lz(blob)
    entries = stage_rle(mid)
    cells, used, stamps = stage_stamps(entries)
    return cells, {
        'compressed': lz_end - file_offset(bank, addr),
        'lz_out': len(blob),
        'entries': len(entries) // 2,
        'entries_used': used // 2,
        'stamps': stamps,
    }


def write_png(path, w, h, rgb):
    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)


def preview_rgb(cells):
    """False-colour preview: enough to recognise coastlines and structure.

    Real tile CHR lives in VRAM and needs the live palette, so this maps the
    10-bit index straight to an arbitrary colour instead: index 0 (the most
    common "empty" cell) one flat colour, low indices another, the rest by
    hash so distinct structures read as distinct colours. These are *not* the
    game's colours and carry no claim about which index means water vs. bare
    land -- that needs the tile table at 02:d6a9 and the live palette.
    """
    out = bytearray()
    for v in cells:
        i = v & 0x03FF
        if i == 0:
            out += bytes((40, 70, 160))
        elif i < 0x40:
            out += bytes((60 + i * 2, 130 + i, 60))
        else:
            h = (i * 2654435761) & 0xFFFFFFFF
            out += bytes((120 + (h & 0x7F), 120 + ((h >> 8) & 0x7F),
                          120 + ((h >> 16) & 0x7F)))
    return bytes(out)


def main():
    if len(sys.argv) > 1:
        rom_path = sys.argv[1]
    else:
        from find_rom import find_rom
        rom_path = find_rom('us')
    if not os.path.exists(rom_path):
        sys.exit('%s not found -- stage your own ROM there first' % rom_path)
    with open(rom_path, 'rb') as f:
        rom = f.read()

    outdir = os.path.join('extracted_assets', 'maps')
    os.makedirs(outdir, exist_ok=True)

    failures = 0
    for name, bank, addr in SCENARIOS:
        try:
            cells, stats = decode_map(rom, bank, addr)
        except Exception as exc:
            print('%-14s %02X:%04X  FAILED: %s' % (name, bank, addr, exc))
            failures += 1
            continue

        bad = sum(1 for v in cells if v > 0x03FF)
        data = b''.join(struct.pack('<H', v) for v in cells)
        with open(os.path.join(outdir, name + '.bin'), 'wb') as f:
            f.write(data)
        write_png(os.path.join(outdir, name + '.png'),
                  MAP_W, MAP_H, preview_rgb(cells))

        print('%-14s %02X:%04X  %5d in -> %5d lz -> %5d entries '
              '(%d used, %d stamps) -> %d bytes%s'
              % (name, bank, addr, stats['compressed'], stats['lz_out'],
                 stats['entries'], stats['entries_used'], stats['stamps'],
                 len(data),
                 '' if not bad else '  [%d cells out of 10-bit range]' % bad))
        if len(data) != MAP_BYTES or bad:
            failures += 1

    print('\nwrote %s' % outdir)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
