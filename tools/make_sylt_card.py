#!/usr/bin/env python3
"""Turn a hand-drawn Sylt card into SNES tiles for the scenario selector.

The card is rendered by the PPU as part of BG1, not by a host overlay, so this
emits 2bpp character data and nothing else. Measured off the live screen:

  * the selector runs mode 0 with BG1 the only layer on the main screen,
    tilemap $3000, wide=1 (64 columns), character base $0000
  * card bodies occupy 8 columns by 9 rows (tilemap rows 5..13), so 64x72,
    with a drop shadow of tile $0010 down the right and along the bottom
  * card art uses CGRAM palette 2, whose four entries are
        0 #000000   1 #94948b   2 #eeeecd   3 #73736a
    i.e. black, mid grey, cream, dark grey -- which is exactly the range a
    greyscale drawing needs, so no per-tile palette juggling is required

Output, little-endian:

    u16 tiles_wide, u16 tiles_high, u16 reserved, u16 reserved
    tiles_wide*tiles_high * 16 bytes   2bpp tile data, row-major

The repo stays asset-free: this writes next to the source image and the host
loads it at runtime, falling back to a drawn placeholder when it is absent.

Usage:
    python tools/make_sylt_card.py sylt_graphics/Untitled.jpg
"""
import os
import struct
import sys

from PIL import Image

TILES_W, TILES_H = 8, 9
PAL = [(0x00, 0x00, 0x00), (0x94, 0x94, 0x8b),
       (0xee, 0xee, 0xcd), (0x73, 0x73, 0x6a)]


def nearest(px):
    best, bi = None, 0
    for i, c in enumerate(PAL):
        d = sum((px[k] - c[k]) ** 2 for k in range(3))
        if best is None or d < best:
            best, bi = d, i
    return bi


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    src = sys.argv[1]
    im = Image.open(src).convert("RGB")

    # BOX, not NEAREST: the source is a JPEG of pixel art and carries
    # compression noise that nearest-neighbour samples straight through.
    # Average first, quantise after, and the shapes survive.
    small = im.resize((TILES_W * 8, TILES_H * 8), Image.BOX)
    px = small.load()

    idx = [[nearest(px[x, y]) for x in range(TILES_W * 8)]
           for y in range(TILES_H * 8)]

    out = bytearray()
    for ty in range(TILES_H):
        for tx in range(TILES_W):
            for y in range(8):
                lo = hi = 0
                for x in range(8):
                    v = idx[ty * 8 + y][tx * 8 + x]
                    bit = 7 - x
                    lo |= (v & 1) << bit
                    hi |= ((v >> 1) & 1) << bit
                out += bytes([lo, hi])

    dst = os.path.join(os.path.dirname(src) or ".", "sylt_card.bin")
    with open(dst, "wb") as f:
        f.write(struct.pack("<HHHH", TILES_W, TILES_H, 0, 0))
        f.write(out)
    print("%s -> %s  (%dx%d tiles, %d bytes)"
          % (src, dst, TILES_W, TILES_H, 8 + len(out)))

    prev = Image.new("RGB", (TILES_W * 8, TILES_H * 8))
    for y in range(TILES_H * 8):
        for x in range(TILES_W * 8):
            prev.putpixel((x, y), PAL[idx[y][x]])
    # Named after the source, NOT a fixed name: the preview is itself a
    # perfectly good source to re-import from once it has been touched up by
    # hand, and writing back over it would destroy the edit.
    stem = os.path.splitext(os.path.basename(src))[0]
    pv = os.path.join(os.path.dirname(src) or ".", stem + "_check.png")
    prev.resize((TILES_W * 8 * 6, TILES_H * 8 * 6), Image.NEAREST).save(pv)
    print("preview -> %s" % pv)


main()
