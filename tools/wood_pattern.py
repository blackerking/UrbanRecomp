#!/usr/bin/env python3
"""Find the repeating block in a background tilemap, and render the match.

Widescreen margins on this host are filled from a screen's own backdrop layer.
Doing that correctly needs the layer's true repeat -- its period in tiles and
the phase the visible picture sits at -- rather than an assumed one. Borrowing
pixels from "16 rows above" was such an assumption, and on the fax it dragged
the machine's structure downward into leg-shaped smears and put the grain out
of step.

This measures the period instead of guessing it, and renders three panels so
the answer can be checked by eye:

    1. the tilemap as the game has it
    2. the detected block, alone
    3. that block tiled back out over the same area

If panels 1 and 3 match, the period is right.

With --margin N it also writes <out>-margins.png: the visible 32 columns with
N tiles of proposed margin grown off each side, so the seam can be judged
before any of it is built into the runner. The margin is grown with the rule
measured here -- see grow() -- not by borrowing pixels.

Usage:
    python tools/wood_pattern.py <ppu_dump> <map_base> <chr_base> <out.png>
                                 [row0 row1] [cgram_base] [--margin N]

    cgram_base is the layer's first CGRAM entry. A 2bpp layer takes four
    colours per palette from there. In BG mode 0 each background gets its own
    range -- BG1 at 0, BG2 at 32, BG3 at 64, BG4 at 96 -- so the fax's BG3
    needs 64. In mode 1 a 2bpp BG3 starts at 0, which is the default.

    row0/row1 restrict the period search to a band of tilemap rows. That
    matters: the fax's tilemap holds the machine as well as the desk, and
    measuring across the whole map reports "32x32", i.e. no repeat at all,
    because the machine is not periodic. Give it the desk's rows and it finds
    the desk's period.

    map_base / chr_base are WORD addresses in hex, as PPU_bgTilemapAdr and
    PPU_bgTileAdr report them -- e.g. the fax's BG3 is 5000 / 0000, the main
    menu's is 3000 / 4000.

The dump is what SC_PPU_DUMP_DIR writes: 64KB VRAM, then 512B CGRAM.
"""
import sys

from PIL import Image

COLS, ROWS = 32, 32
SCREEN_ROWS = 28          # visible rows; the map is 32 but four never show

# The wood sheet is tiles $020..$11f, laid out sixteen to a row of the sheet.
# Both the fax and the main menu draw from it, and both were confirmed to
# render it pixel-identically, so one description covers them.
WOOD_LO, WOOD_HI = 0x020, 0x11F


def load(path):
    d = open(path, "rb").read()
    vram = [d[i] | (d[i + 1] << 8) for i in range(0, 0x10000, 2)]
    cg = d[0x10000:0x10200]
    return vram, cg


def rgb(cg, i):
    v = cg[i * 2] | (cg[i * 2 + 1] << 8)
    return ((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31,
            ((v >> 10) & 31) * 255 // 31)


def tile_pixels(vram, cg, entry, chr_base, cg_base=0):
    """One 2bpp tile -> 8x8 RGB, honouring the entry's palette and flips."""
    t = entry & 0x3FF
    pal = (entry >> 10) & 7
    xf, yf = bool(entry & 0x4000), bool(entry & 0x8000)
    out = []
    for y in range(8):
        sy = 7 - y if yf else y
        w = vram[chr_base + t * 8 + sy]
        lo, hi = w & 0xFF, w >> 8
        row = []
        for x in range(8):
            sx = x if xf else 7 - x
            row.append(rgb(cg, cg_base + pal * 4 +
                           (((lo >> sx) & 1) | (((hi >> sx) & 1) << 1))))
        out.append(row)
    return out


def detect_period(ent, r0=0, r1=ROWS):
    """Smallest (pw, ph) with ent[r][c] == ent[r+ph][c+pw] over rows [r0,r1)."""
    def ok_w(pw):
        return all(ent[r][c] == ent[r][c + pw]
                   for r in range(r0, r1) for c in range(COLS - pw))

    def ok_h(ph):
        return all(ent[r][c] == ent[r + ph][c]
                   for r in range(r0, r1 - ph) for c in range(COLS))

    pw = next((p for p in range(1, COLS + 1) if ok_w(p)), COLS)
    ph = next((p for p in range(1, ROWS + 1) if ok_h(p)), ROWS)
    return pw, ph


def is_wood(entry):
    return WOOD_LO <= (entry & 0x3FF) <= WOOD_HI and not (entry & 0xC000)


def grow(entry, d):
    """The tile d columns further along the wood, from the tile at an edge.

    Each row of the sheet is sixteen consecutive tiles, so walking sideways
    means walking the low four bits and wrapping them; the high bits pick the
    sheet row and stay put. Reading it off the edge tile this way carries the
    screen's own phase with it, which is what makes the seam disappear -- the
    fax and the menu sit at different phases and neither had to be told which.
    """
    t = entry & 0x3FF
    return (entry & ~0x3FF) | (t & ~0x0F) | ((t + d) & 0x0F)


def wood_run_at(ent, r, c, step, n=4):
    """Is there a run of n consecutive wood tiles from c, walking by step?

    "The end tile is in the wood range" was too weak a test to grow from. On
    map select the scenario names sit in the tilemap off to both sides, and
    their font tiles fall inside the same range, so growing from one walked
    along the font and printed "Flooding" and "Coastal" into the right margin.
    Letters are not consecutive tile numbers -- "Flooding" repeats an o -- so
    demanding a real run rejects them while every wood row still passes.
    """
    for k in range(n):
        a = ent[r][c + k * step]
        if not is_wood(a):
            return False
        if k and a != grow(ent[r][c + (k - 1) * step], step):
            return False
    return True


def wood_row_ends(ent, r):
    return wood_run_at(ent, r, 0, 1) and wood_run_at(ent, r, COLS - 1, -1)


def wood_row(ent, r, vp):
    """The row to grow a margin from: r itself, or the wood row it repeats.

    Rows carrying furniture -- the fax machine spans rows 19..30 -- have no
    wood at their edges to take a phase from, so they fall back to the row the
    wood repeats from vp rows up. That is the whole of the old "borrow from 16
    rows above" idea, kept only for the rows that actually need it, and applied
    to tilemap entries rather than to finished pixels, so nothing structural
    can be dragged sideways into the margin.
    """
    if wood_row_ends(ent, r):
        return r
    return r % vp


def render(ent, vram, cg, chr_base, cols, rows, src=None, cg_base=0):
    im = Image.new("RGB", (cols * 8, rows * 8))
    for r in range(rows):
        for c in range(cols):
            e = ent[r][c] if src is None else src(r, c)
            px = tile_pixels(vram, cg, e, chr_base, cg_base)
            for y in range(8):
                for x in range(8):
                    im.putpixel((c * 8 + x, r * 8 + y), px[y][x])
    return im


def render_margins(ent, vram, cg, chr_base, cg_base, m, vp, out):
    """Visible 32 columns with m tiles of grown margin either side."""
    total = COLS + 2 * m

    def src(r, c):
        if m <= c < m + COLS:
            return ent[r][c - m]
        w = wood_row(ent, r, vp)
        if c < m:
            return grow(ent[w][0], c - m)              # c-m is negative
        return grow(ent[w][COLS - 1], c - (m + COLS) + 1)

    im = render(ent, vram, cg, chr_base, total, SCREEN_ROWS,
                src=src, cg_base=cg_base)
    im = im.resize((total * 8 * 3, SCREEN_ROWS * 8 * 3), Image.NEAREST)
    # Hairlines on the two seams, so a mismatch cannot hide behind the eye
    # deciding where the authentic picture ended.
    for x in (m * 8 * 3, (m + COLS) * 8 * 3 - 1):
        for y in range(im.height):
            im.putpixel((x, y), (0, 255, 255) if (y // 6) % 2 else (0, 0, 0))
    im.save(out)
    print("wrote %s  (cyan hairlines mark the authentic 256px edges)" % out)


def main():
    args = [a for a in sys.argv[1:] if a != "--margin"]
    margin = 0
    if "--margin" in sys.argv:
        i = sys.argv.index("--margin")
        margin = int(sys.argv[i + 1])
        args.remove(sys.argv[i + 1])
    if len(args) < 4:
        raise SystemExit(__doc__)
    dump, map_base, chr_base, out = args[:4]
    map_base, chr_base = int(map_base, 16), int(chr_base, 16)
    r0 = int(args[4]) if len(args) > 4 else 0
    r1 = int(args[5]) if len(args) > 5 else ROWS
    cg_base = int(args[6]) if len(args) > 6 else 0
    vram, cg = load(dump)

    ent = [[vram[map_base + r * COLS + c] for c in range(COLS)] for r in range(ROWS)]
    pw, ph = detect_period(ent, r0, r1)
    print("detected period: %d tiles wide x %d tiles tall (%d x %d pixels)"
          % (pw, ph, pw * 8, ph * 8))

    orig = render(ent, vram, cg, chr_base, COLS, ROWS, cg_base=cg_base)
    block = render(ent, vram, cg, chr_base, pw, ph,
                   src=lambda r, c: ent[r0 + r][c], cg_base=cg_base)
    tiled = render(ent, vram, cg, chr_base, COLS, ROWS,
                   src=lambda r, c: ent[r0 + (r - r0) % ph][c % pw], cg_base=cg_base)

    same = sum(1 for r in range(r0, r1) for c in range(COLS)
               if ent[r][c] == ent[r0 + (r - r0) % ph][c % pw])
    print("tiling the block reproduces %d of %d cells in rows %d..%d"
          % (same, (r1 - r0) * COLS, r0, r1 - 1))

    pad = 8
    w = COLS * 8 * 2 + pw * 8 + pad * 2
    im = Image.new("RGB", (w, ROWS * 8), (0, 0, 0))
    im.paste(orig, (0, 0))
    im.paste(block, (COLS * 8 + pad, 0))
    im.paste(tiled, (COLS * 8 + pad + pw * 8 + pad, 0))
    im.resize((w * 2, ROWS * 8 * 2), Image.NEAREST).save(out)
    print("wrote %s  (original | detected block | block tiled back)" % out)

    if margin:
        stem = out[:-4] if out.lower().endswith(".png") else out
        render_margins(ent, vram, cg, chr_base, cg_base, margin, ph,
                       stem + "-margins.png")


main()
