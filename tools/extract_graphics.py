#!/usr/bin/env python3
"""Extract SimCity (SNES) compressed graphics and dialog text as PNG/bin files.

Decodes the Nintendo compression format used throughout this ROM for the
font tileset and all advisor/scenario dialog text (sometimes called
LC_LZ5; likely shared with other Nintendo-published SNES titles). The
decompressor itself lives at $0090DD in the US ROM this project targets
(see docs/REFERENCE_third_party_optimization_patch.md for how that
address was found, and $0090A6 in the Japanese ROM per community
reverse-engineering).

This is an offline, ROM-file-only tool -- it never touches a running
emulator, so there's no risk of corrupting live game state the way a
runtime HLE override could (see docs/REFERENCE_third_party_optimization_patch.md's
notes on why that approach was avoided for map-loading speed).

Usage:
    python tools/extract_graphics.py [--rom PATH] [--out DIR] [--version {us,jp,eu,fr,de}]

Requires Pillow (`pip install Pillow`). Output is never committed to this
repo (see .gitignore) since it's derived from the copyrighted ROM.
"""
import argparse
import os
import sys

try:
    import PIL.Image
except ImportError:
    sys.exit("This tool needs Pillow: pip install Pillow")

# ── per-region ROM offsets (community-sourced; only 'us' has been
#    verified against this project's own ROM) ───────────────────────────
VERSIONS = {
    "jp": dict(
        tiledata=0x06E004, textdata=[0x6F4E8, 0x7126B],
        tilescen=0x047C9E, textscen=[(0x05BB27, 5), (0x05F620, 12)],
        jp=True, ascii_offset=0, cols=24, scencols=32, paircols=32,
    ),
    "us": dict(
        tiledata=0x04C0FB, textdata=[0x7A868, 0x7DA83],
        tilescen=0x04875C, textscen=[(0x05BCAD, 5), (0x05EE30, 12)],
        jp=False, ascii_offset=0, cols=24, scencols=32, paircols=16,
    ),
    "eu": dict(
        tiledata=0x04C10F, textdata=[0x7A868, 0x7DA83],
        tilescen=0x04875D, textscen=[(0x05BD54, 5), (0x05EED7, 12)],
        jp=False, ascii_offset=0, cols=24, scencols=32, paircols=16,
    ),
    "fr": dict(
        tiledata=0x04C601, textdata=[0x7B068, 0x7E40E],
        tilescen=0x048AF2, textscen=[(0x05C9FE, 5), (0x05FC05, 12)],
        jp=False, ascii_offset=0x20, cols=25, scencols=32, paircols=16,
    ),
    "de": dict(
        tiledata=0x04C223, textdata=[0x7B068, 0x7E399],
        tilescen=0x048737, textscen=[(0x05CBE6, 5), (0x05FDCE, 12)],
        jp=False, ascii_offset=0x20, cols=25, scencols=32, paircols=16,
    ),
}

PALETTE = [
    0xEE, 0xE2, 0xDE,  # 0 background
    0x00, 0x00, 0x00,  # 1 text
    0xFF, 0x00, 0x00,  # 2 error
    0xDE, 0xC6, 0xBD,  # 3 grid
    0xDE, 0xFF, 0xBD,  # 4 reused tile background
    0x00, 0xA0, 0x00,  # 5 reused tile text
    0xEE, 0xE2, 0xDE,  # 6 2-bit background
    0xFF, 0x00, 0x00,  # 7 2-bit text 0
    0x00, 0x00, 0xFF,  # 8 2-bit text 1
    0x00, 0x00, 0x00,  # 9 2-bit text 2
]


class DecompError(Exception):
    pass


def nintendo_decompress(rom, offset):
    """Decode one compressed packet at `offset`.

    Returns (decompressed_bytes, offset_past_packet). Raises DecompError
    on malformed input -- callers doing an exploratory scan should catch
    this rather than let it propagate.

    Packet format (control byte $FF ends the stream; otherwise top 3 bits
    select a mode, bottom 5 bits are length-1, extendable to 10 bits via
    the $E0 prefix mode):
      000 copy N literal bytes
      001 repeat 1 byte N times
      010 repeat a 2-byte pair for N bytes
      011 emit N bytes counting up from a given start
      100 copy N bytes from an absolute offset into the output so far
      101 same, XORing $FF into each copied byte
      110 copy N bytes from `output_length - offset` bytes back
      111 extended-length prefix for any of the above (or, with mode
          bits 111, relative-reference + invert)
    """
    pos = offset

    def next_byte():
        nonlocal pos
        if pos >= len(rom):
            raise DecompError("out of data at 0x%06X" % pos)
        b = rom[pos]
        pos += 1
        return b

    out = bytearray()
    while True:
        control = next_byte()
        if control == 0xFF:
            break
        mode = control & 0xE0
        length = control & 0x1F
        if mode == 0xE0:
            mode = (control << 3) & 0xE0
            length = next_byte() | ((control & 0x03) << 8)
        length += 1

        if mode == 0x00:  # copy
            out.extend(next_byte() for _ in range(length))
        elif mode == 0x20:  # byte repeat
            r = next_byte()
            out.extend([r] * length)
        elif mode == 0x40:  # word repeat
            r0, r1 = next_byte(), next_byte()
            out.extend(r0 if i % 2 == 0 else r1 for i in range(length))
        elif mode == 0x60:  # incrementing
            r = next_byte()
            for _ in range(length):
                out.append(r)
                r = (r + 1) & 0xFF
        else:  # 0x80/0xA0/0xC0/0xE0-extended: reference modes
            if mode & 0x40:  # relative (0xC0/0xE0)
                back = next_byte()
                ref = len(out) - back
            else:  # absolute (0x80/0xA0)
                lo, hi = next_byte(), next_byte()
                ref = lo | (hi << 8)
            invert = bool(mode & 0x20)
            if ref < 0:
                raise DecompError("relative ref out of range at 0x%06X" % pos)
            for _ in range(length):
                if ref >= len(out):
                    raise DecompError("reference out of range at 0x%06X" % pos)
                b = out[ref]
                ref += 1
                out.append(b ^ 0xFF if invert else b)
    return bytes(out), pos


def render_text(tiledata, textdata, offset=0, columns=24, rows=None, grid=False, reuse=False):
    """Render a block of 16-bit tile-index text as an image (1bpp font)."""
    stride = columns * 2 * 2
    if rows is None:
        rows = max(1, (len(textdata) - offset + stride - 1) // stride)
    iw, ih = 8 * columns, 16 * rows
    clear = 2
    if grid:
        iw += 1 + columns
        ih += 1 + rows
        clear = 3
    img = PIL.Image.new("P", (max(iw, 1), max(ih, 1)), clear)
    img.putpalette(PALETTE)
    reused = set()
    for rd in range(rows):
        for rh in range(2):
            r = rd * 2 + rh
            for c in range(columns):
                do = offset + (r * columns + c) * 2
                if do + 2 > len(textdata):
                    continue
                ti = textdata[do] | (textdata[do + 1] << 8)
                if (ti * 8 + 8) > len(tiledata):
                    continue
                ox, oy = c * 8, r * 8
                if grid:
                    ox += 1 + c
                    oy += 1 + (r // 2)
                for y in range(8):
                    bits = tiledata[ti * 8 + y]
                    for x in range(8):
                        p = (bits >> (7 - x)) & 1
                        if ti in reused:
                            p += 4
                        img.putpixel((ox + x, oy + y), p)
                if reuse:
                    reused.add(ti)
    return img


def render_tileset(tiledata, columns=16):
    tiles = max(1, (len(tiledata) + 7) // 8)
    rows = (tiles + columns - 1) // columns
    img = PIL.Image.new("P", (1 + 9 * columns, 1 + 9 * rows), 3)
    img.putpalette(PALETTE)
    for r in range(rows):
        for c in range(columns):
            ti = c + r * columns
            ox, oy = 1 + 9 * c, 1 + 9 * r
            for y in range(8):
                do = ti * 8 + y
                if do >= len(tiledata):
                    continue
                bits = tiledata[do]
                for x in range(8):
                    img.putpixel((ox + x, oy + y), (bits >> (7 - x)) & 1)
    return img


def tileset_reduce(tiledata):
    """Collapse a 2bpp non-Japanese font tileset down to 1bpp."""
    return bytes((tiledata[i] | tiledata[i + 1]) ^ 0xFF for i in range(0, len(tiledata) - 1, 2))


def tilescen_reduce(tilescen):
    return bytes(tilescen[i] for i in range(0, len(tilescen) - 1, 2))


def textscen_convert(textscen):
    out = bytearray()
    for i in range(0, len(textscen) - 1, 2):
        ti = (textscen[i] | (textscen[i + 1] << 8)) % 1024
        out += bytes([ti & 0xFF, ti >> 8])
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", default=os.path.join(os.path.dirname(__file__), "..", "simcity.sfc"))
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "extracted_assets"))
    ap.add_argument("--version", default="us", choices=sorted(VERSIONS))
    args = ap.parse_args()

    if not os.path.exists(args.rom):
        sys.exit("ROM not found at %s -- supply your own legally obtained copy (see SETUP.md)" % args.rom)
    rom = open(args.rom, "rb").read()
    cfg = VERSIONS[args.version]
    os.makedirs(args.out, exist_ok=True)

    def outpath(name):
        return os.path.join(args.out, name)

    tiledata_raw, tiledata_end = nintendo_decompress(rom, cfg["tiledata"])
    tiledata = tiledata_raw if cfg["jp"] else tileset_reduce(tiledata_raw)
    render_tileset(tiledata).save(outpath("font_tileset.png"))
    open(outpath("font_tileset.bin"), "wb").write(tiledata_raw)
    print("font tileset: %d bytes (%d compressed) -> font_tileset.png/.bin" %
          (len(tiledata_raw), tiledata_end - cfg["tiledata"]))

    if cfg["jp"]:
        for i, off in enumerate(cfg["textdata"]):
            d, end = nintendo_decompress(rom, off)
            name = "dialog_text_%d" % i
            open(outpath(name + ".bin"), "wb").write(d)
            render_text(tiledata, d, columns=cfg["cols"]).save(outpath(name + ".png"))
            print("dialog text [0x%06X]: %d bytes (%d compressed) -> %s.png/.bin" %
                  (off, len(d), end - off, name))
    else:
        lo, hi = cfg["textdata"]
        d = rom[lo:hi]
        open(outpath("dialog_text.bin"), "wb").write(d)
        # non-JP releases store plain ASCII (minus an optional per-locale
        # offset); expand each byte to a 16-bit tile index for rendering
        expanded = bytearray()
        col = 0
        for b in d:
            if b != 0xFF:
                v = b - cfg["ascii_offset"]
                expanded += bytes([v & 0xFF, 0]) if v >= 0 else bytes([0xFF, 0xFF])
                col = (col + 1) % cfg["cols"]
            else:
                while col != 0:
                    expanded += bytes([ord(" "), 0])
                    col = (col + 1) % cfg["cols"]
                expanded += bytes([0xFF, 0xFF]) * cfg["cols"]
        render_text(tiledata, expanded, columns=cfg["cols"]).save(outpath("dialog_text.png"))
        print("dialog text [0x%06X-0x%06X]: %d bytes -> dialog_text.png/.bin" % (lo, hi, len(d)))

    tilescen_raw, tilescen_end = nintendo_decompress(rom, cfg["tilescen"])
    tilescen = tilescen_reduce(tilescen_raw)
    render_tileset(tilescen).save(outpath("scenario_tileset.png"))
    open(outpath("scenario_tileset.bin"), "wb").write(tilescen_raw)
    print("scenario tileset: %d bytes (%d compressed) -> scenario_tileset.png/.bin" %
          (len(tilescen_raw), tilescen_end - cfg["tilescen"]))

    for group_idx, (off, count) in enumerate(cfg["textscen"]):
        cur = off
        for i in range(count):
            d, end = nintendo_decompress(rom, cur)
            name = "scenario_text_g%d_%d" % (group_idx, i)
            open(outpath(name + ".bin"), "wb").write(d)
            render_text(tilescen, textscen_convert(d), columns=cfg["scencols"]).save(outpath(name + ".png"))
            print("scenario text [0x%06X]: %d bytes (%d compressed) -> %s.png/.bin" %
                  (cur, len(d), end - cur, name))
            cur = end

    print("\nDone. Output in %s (not committed -- derived from the copyrighted ROM)." % args.out)


if __name__ == "__main__":
    main()
