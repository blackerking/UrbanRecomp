#!/usr/bin/env python3
"""Convert a hand-drawn Sylt card image into the raw form main.c loads.

The repo is deliberately asset-free, so this does NOT generate a C array to be
committed: it writes a small binary next to the source image, and the host
loads it at runtime and falls back to a drawn placeholder when it is absent.

Input is the whole card as drawn (thumbnail plus its caption). Only the
thumbnail is taken -- the caption is redrawn by the host in its own font, so
the label and year stay editable without regenerating anything.

Output format, little-endian:

    u16 width, u16 height, u16 palette_count, u16 reserved
    palette_count * 3 bytes   R, G, B
    width * height bytes      palette indices

Usage:
    python tools/make_sylt_card.py sylt_graphics/Untitled.jpg
"""
import struct
import sys
import os

from PIL import Image

# The ROM's own card thumbnails measure 48x40 on the scenario selector; the
# host card is drawn to the same proportions, so match them exactly.
THUMB_W, THUMB_H = 48, 40
PALETTE = 8


def crop_thumbnail(im):
    """Find the thumbnail block inside the card.

    The card body is cream and the thumbnail is everything that is not, in the
    upper part of the image. Searching only the top 60% keeps the caption --
    which is also not cream -- out of the box.
    """
    px = im.load()
    w, h = im.size

    def cream(c):
        return c[0] > 200 and c[1] > 200 and c[2] > 180

    # Rows belonging to the thumbnail are almost entirely non-cream, because
    # it is a solid block. Caption rows are mostly cream with letters in them.
    # Thresholding on that separates the two without guessing a split point --
    # cropping on the bounding box alone dragged the top of "Sylt" in.
    rows = []
    for y in range(0, h):
        n = sum(1 for x in range(0, w, 2) if not cream(px[x, y]))
        rows.append(n / float(len(range(0, w, 2))))
    # 0.60 of the FULL image width: the thumbnail spans about 75% of it,
    # while caption rows sit near 0.30, so this sits clear of both.
    solid = [y for y, frac in enumerate(rows) if frac > 0.60]
    if not solid:
        raise SystemExit("no solid block found -- is this a card image?")
    # Take the first contiguous run, which is the thumbnail.
    y0 = solid[0]
    y1 = y0
    for y in solid:
        if y - y1 > 2:
            break
        y1 = y
    xs = [x for x in range(w)
          if any(not cream(px[x, y]) for y in range(y0, y1 + 1, 2))]
    return im.crop((min(xs), y0, max(xs) + 1, y1 + 1))


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    src = sys.argv[1]
    im = Image.open(src).convert("RGB")
    thumb = crop_thumbnail(im)

    # BOX, not NEAREST: the source is a JPEG of pixel art, so it carries
    # compression noise that nearest-neighbour would sample straight through.
    # Averaging first and quantising after keeps the shapes.
    small = thumb.resize((THUMB_W, THUMB_H), Image.BOX)
    q = small.quantize(colors=PALETTE, method=Image.MEDIANCUT)
    pal = q.getpalette()[: PALETTE * 3]
    idx = bytes(q.getdata())

    out = os.path.join(os.path.dirname(src) or ".", "sylt_card.bin")
    with open(out, "wb") as f:
        f.write(struct.pack("<HHHH", THUMB_W, THUMB_H, PALETTE, 0))
        f.write(bytes(pal))
        f.write(idx)
    print("%s -> %s  (%dx%d, %d colours, %d bytes)"
          % (src, out, THUMB_W, THUMB_H, PALETTE, 8 + len(pal) + len(idx)))

    preview = q.convert("RGB").resize((THUMB_W * 6, THUMB_H * 6), Image.NEAREST)
    pv = os.path.join(os.path.dirname(src) or ".", "sylt_card_preview.png")
    preview.save(pv)
    print("preview -> %s" % pv)


main()
