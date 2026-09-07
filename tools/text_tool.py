#!/usr/bin/env python3
"""Export SimCity (SNES) message text for translation, and pack it back.

The goal is that somebody who does not read 65816 can translate the game.
`export` writes a UTF-8 JSON file with one entry per message; `pack` turns an
edited file back into the raw block the game reads.

Round-tripping is checked, not assumed: `export` re-packs what it just wrote
and refuses to produce a file that would not come back byte-identical.

The message block is 53 records separated by $FF, and the record INDEX is the
same message in every region -- verified across us/eu/fr/de -- so an entry
exported from one language can be translated against any other.

Nothing here touches a running emulator, and no output is ever committed:
it is derived from a copyrighted ROM. See .gitignore.

Usage:
    python tools/text_tool.py export --rom ROM --version us --out text_us.json
    python tools/text_tool.py pack   --in text_de.json --out text_de.bin
    python tools/text_tool.py glyphs --rom ROM --version us

Regions: us eu fr de  (jp is tile-indexed, not characters -- see --help)
"""
import argparse, json, os, sys

SEP = 0xFF

# Dialog block, per region. Offsets are the ones extract_graphics.py uses.
# Taken from extract_graphics.py's table, NOT assumed shared: fr and de put
# their text in a different place, and the first version of this file used the
# us range for all four. The round-trip check did not catch it -- garbage
# round-trips perfectly well -- which is why load_block() sanity-checks the
# record count instead.
BLOCKS = {
    "us": (0x07A868, 0x07DA83),
    "eu": (0x07A868, 0x07DA83),
    "fr": (0x07B068, 0x07E40E),
    "de": (0x07B068, 0x07E399),
}

# Every region ships the same 53 messages, so a wrong offset shows up at once.
EXPECT_RECORDS = 53

# We run the US ROM and nothing else: every ROM-address hook in main.c, the AOT
# tier and the host map renderer are all keyed to it. Other ROMs are donors --
# their text is lifted out and applied over the US image, so a player gets
# German text with the US build's features intact.
TARGET = "us"
TARGET_OFF = 0x07A868

# The US block is 12827 bytes, but 9597 bytes of $FF filler follow it, running
# exactly to the $080000 bank boundary. Translations longer than the English
# original spill into that -- German needs +278, French +395. The extra $FF
# bytes read as empty records past the last real one, which nothing asks for.
# NB: "unused" is inferred from the fill pattern, not proven.
TARGET_BUDGET = 0x080000 - TARGET_OFF          # 22424

MAGIC = b"SCTR"                                 # blob header, 12 bytes
HDR = 12

REGION_BYTE = {0x01: "us", 0x02: "eu", 0x06: "fr", 0x09: "de"}


# ── scenario briefings ───────────────────────────────────────────────────
# Not ASCII: each cell is a 16-bit tilemap entry into the scenario tileset,
# with SEPARATE bases for upper and lower case. That is why every search for a
# single base failed. The US bases are the pair brief_put() in main.c already
# writes for Sylt; the donors sit $10 higher, so a donor tilemap cannot just be
# copied -- it is decoded with its own bases and re-encoded with ours.
# Derived by correlating each region's tile frequencies against its language's
# letter frequencies, at step 1. An earlier search stepped by 0x10 and could
# only return multiples of 16: it reported $6a0 for de/fr, one short of the
# truth, which shifts every letter by one and renders pure gibberish. It still
# round-tripped perfectly, because a constant offset is self-consistent --
# consistency is not correctness.
BRIEF_BASES = {"us": 0x690, "eu": 0x690, "de": 0x6a1, "fr": 0x6a1}
BRIEF_BLANK = 0x3ff

# Some regions use a SECOND tile for the space between words, keeping $3ff for
# empty cells. German's is $690, which occurs 801 times across the briefings --
# more often than "e" at 522, and the US briefings have no comparable
# undecoded tile (their most frequent is 16 uses). A glyph that frequent in
# running text is a space. Without this the words run together.
BRIEF_SPACE_ALIAS = {"de": 0x690, "fr": 0x690}
BRIEF_COLS = 32
KEEP = "�"          # a cell that is not a character: left exactly as it was


def brief_dec(t, up, space_alias=None):
    lo = up + 0x30
    if space_alias is not None and t == space_alias:
        return " "
    if up <= t < up + 26:            return chr(ord("A") + t - up)
    if lo <= t < lo + 26:            return chr(ord("a") + t - lo)
    if up + 0x20 <= t < up + 0x2a:   return chr(ord("0") + t - up - 0x20)
    if t == up + 0x1c:               return ","
    if t == up + 0x1d:               return "."
    if t == up + 0x1e:               return "'"
    if t == BRIEF_BLANK:             return " "
    return KEEP


def brief_enc(c, up):
    lo = up + 0x30
    if "A" <= c <= "Z":  return up + ord(c) - ord("A")
    if "a" <= c <= "z":  return lo + ord(c) - ord("a")
    if "0" <= c <= "9":  return up + 0x20 + ord(c) - ord("0")
    return {",": up + 0x1c, ".": up + 0x1d, "'": up + 0x1e, " ": BRIEF_BLANK}.get(c)


def brief_packets(rom_path, version):
    """Decompressed briefing packets, with the ROM address each came from."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "eg", os.path.join(os.path.dirname(__file__), "extract_graphics.py"))
    eg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(eg)
    rom = open(rom_path, "rb").read()
    off, cnt = eg.VERSIONS[version]["textscen"][1]
    out = []
    o = off
    for _ in range(cnt):
        start = o
        d, o = eg.nintendo_decompress(rom, o)
        out.append(((start // 0x8000) << 16 | ((start % 0x8000) + 0x8000), d))
    return out


def brief_to_rows(data, up, space_alias=None):
    import struct
    w = struct.unpack("<%dH" % (len(data) // 2), data[:len(data) // 2 * 2])
    rows = []
    for r in range(0, len(w), BRIEF_COLS):
        rows.append("".join(brief_dec(t, up, space_alias) for t in w[r:r + BRIEF_COLS]))
    return rows, w


def brief_from_rows(rows, original, up):
    """Re-encode, keeping any cell the decoder could not read as a character."""
    import struct
    out = list(original)
    i = 0
    for row in rows:
        for c in row:
            if i >= len(out):
                break
            if c != KEEP:
                t = brief_enc(c, up)
                if t is not None:
                    out[i] = t
            i += 1
    return struct.pack("<%dH" % len(out), *out)


# ── scenario picture tiles ───────────────────────────────────────────────
# The scenario cards and the HUD word-strips are ARTWORK, not text: pictures of
# words, so there is no string to substitute. 1024 tiles of 16 bytes (2bpp),
# compressed in the ROM, at a different address per region. 67% of them differ
# between US and German -- that difference IS the translation.
#
# Two ways to translate them. Copy a donor ROM's tileset wholesale, which needs
# nobody to draw anything; or edit the exported .bin and hand it back. The .png
# is a reference view only: it is reduced to 1bpp for legibility and cannot be
# converted back without losing a bitplane, so the .bin is the editable form.
TILESCEN_RAW = 16384


def scen_tiles(rom_path, version):
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "eg", os.path.join(os.path.dirname(__file__), "extract_graphics.py"))
    eg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(eg)
    rom = open(rom_path, "rb").read()
    off = eg.VERSIONS[version]["tilescen"]
    raw, _ = eg.nintendo_decompress(rom, off)
    return raw, off, eg


def cmd_tiles(a):
    ver = detect_region(a.rom)
    raw, off, eg = scen_tiles(a.rom, ver)
    os.makedirs(a.out, exist_ok=True)
    binp = os.path.join(a.out, "scenario_tiles_%s.bin" % ver)
    open(binp, "wb").write(raw)
    try:
        eg.render_tileset(eg.tileset_reduce(raw)).save(
            os.path.join(a.out, "scenario_tiles_%s.png" % ver))
        png = " and .png (reference only, 1bpp)"
    except Exception:
        png = " (.png needs Pillow)"
    print("%s scenario tiles: %d tiles from $%06X" % (ver, len(raw) // 16, off))
    print("  -> %s%s" % (binp, png))
    print("  edit the .bin and pass it back with:  import --tiles-from %s" % binp)


# ── accent glyphs ────────────────────────────────────────────────────────
# German and French text uses 16 characters the US font has no glyph for. All
# 16 slots are FREE in the US font and all 16 are drawn in the donors, so they
# copy straight across with no code remapping: the dialog renderer indexes the
# font by character code, US at offset 0 and the donors at 32.
FONT_RAW_TILE = 16          # stored 2bpp, 16 bytes a tile
ACCENT_CODES = [0x81, 0x82, 0x83, 0x84, 0x85, 0x87, 0x88, 0x8A,
                0x8C, 0x8E, 0x93, 0x94, 0x96, 0x97, 0x9A, 0x9B]


def font_raw(rom_path, version):
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "eg", os.path.join(os.path.dirname(__file__), "extract_graphics.py"))
    eg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(eg)
    rom = open(rom_path, "rb").read()
    cfg = eg.VERSIONS[version]
    raw, _ = eg.nintendo_decompress(rom, cfg["tiledata"])
    return raw, cfg["ascii_offset"]


def accent_glyphs(donor_rom, donor, us_rom):
    """(code, 16 raw bytes) for each accent the donor draws and we lack."""
    dn, dn_off = font_raw(donor_rom, donor)
    us, us_off = font_raw(us_rom, TARGET)
    out = []
    for code in ACCENT_CODES:
        di = code - dn_off
        ui = code - us_off
        if (di + 1) * FONT_RAW_TILE > len(dn):
            continue
        glyph = dn[di * FONT_RAW_TILE:(di + 1) * FONT_RAW_TILE]
        if glyph == bytes(FONT_RAW_TILE):
            continue                      # donor has nothing there either
        out.append((ui, glyph))
    return out


def detect_region(rom_path):
    rom = open(rom_path, "rb").read()
    if len(rom) < 0x8000:
        sys.exit("%s is too small to be a SimCity ROM" % rom_path)
    b = rom[0x7fd9]
    if b not in REGION_BYTE:
        sys.exit("%s: region byte $%02X is not one this tool knows "
                 "(Japan stores text as tile indices, not characters)" % (rom_path, b))
    return REGION_BYTE[b]


def make_blob(records, briefs=(), tiles=None, glyphs=()):
    """Header + the packed message block + any briefing packets.

    v2 layout: "SCTR", ver, region, record count, text length, then the text,
    then a briefing count and, per packet, its 24-bit source address, its
    length, and the re-encoded tilemap. Briefings are keyed by the address
    they decompress FROM, because that is what the runtime hook at 00:9106
    can recognise.
    """
    body = pack_records(records)
    if len(body) > TARGET_BUDGET:
        sys.exit("translation needs %d bytes; the US image has room for %d. "
                 "Shorten the longest messages." % (len(body), TARGET_BUDGET))
    out = (MAGIC + bytes([4, 0x01])
           + len(records).to_bytes(2, "little")
           + len(body).to_bytes(4, "little") + body)
    out += len(briefs).to_bytes(2, "little")
    for src, data in briefs:
        out += src.to_bytes(4, "little") + len(data).to_bytes(4, "little") + data
    out += (len(tiles) if tiles else 0).to_bytes(4, "little")
    if tiles:
        out += tiles
    out += len(glyphs).to_bytes(2, "little")
    for idx, g in glyphs:
        out += idx.to_bytes(2, "little") + g
    return out

# The font is a codepage, not Latin-1: the accented glyphs sit where CP437
# puts them. Decoding as CP437 gives a translator real characters to edit
# instead of escape codes, and it round-trips exactly.
CODEC = "cp437"


def load_block(rom_path, version):
    if version not in BLOCKS:
        sys.exit("version must be one of: %s "
                 "(jp text is tile-indexed, not characters)" % " ".join(BLOCKS))
    rom = open(rom_path, "rb").read()
    lo, hi = BLOCKS[version]
    if hi > len(rom):
        sys.exit("ROM is shorter than the text block -- wrong file?")
    blob = rom[lo:hi]
    n = blob.count(bytes([SEP])) + 1
    if n != EXPECT_RECORDS:
        sys.exit("got %d records from %s at $%06X, expected %d -- wrong offset "
                 "or wrong --version for this ROM" % (n, version, lo, EXPECT_RECORDS))
    return blob


def split_records(blob):
    return blob.split(bytes([SEP]))


def pack_records(records):
    return bytes([SEP]).join(records)


def to_text(rec):
    return rec.decode(CODEC)


def from_text(s):
    return s.encode(CODEC)


def cmd_export(a):
    blob = load_block(a.rom, a.version)
    recs = split_records(blob)
    entries = [{"id": i, "text": to_text(r)} for i, r in enumerate(recs)]

    # Prove the round trip before writing anything.
    back = pack_records([from_text(e["text"]) for e in entries])
    if back != blob:
        sys.exit("refusing to write: this block does not round-trip "
                 "(%d bytes in, %d out)" % (len(blob), len(back)))

    lo, hi = BLOCKS[a.version]
    doc = {
        "_readme": [
            "One entry per in-game message. Translate the \"text\" field only;",
            "leave \"id\" alone -- it is what maps the message back into place.",
            "The game renders into a fixed-width box, so the runs of spaces are",
            "layout, not padding to strip. Keep a line's visible width the same",
            "and it will lay out the way the original did.",
            "Only characters the game has a glyph for will show: run",
            "  python tools/text_tool.py glyphs --rom ROM --version %s" % a.version,
            "to list them. Anything else has no glyph and will not render.",
        ],
        "version": a.version,
        "block_offset": lo,
        "block_size": hi - lo,
        "records": len(entries),
        "entries": entries,
    }
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
    print("exported %d messages -> %s (round-trip verified)" % (len(entries), a.out))


def cmd_pack(a):
    doc = json.load(open(getattr(a, "in"), encoding="utf-8"))
    entries = sorted(doc["entries"], key=lambda e: e["id"])
    if [e["id"] for e in entries] != list(range(len(entries))):
        sys.exit("entry ids must be 0..n-1 with none missing or repeated")
    bad = []
    recs = []
    for e in entries:
        try:
            recs.append(from_text(e["text"]))
        except UnicodeEncodeError as ex:
            bad.append((e["id"], e["text"][ex.start:ex.end]))
    if bad:
        for i, ch in bad[:10]:
            print("  entry %d: no glyph for %r" % (i, ch), file=sys.stderr)
        sys.exit("%d entr%s use characters the game cannot draw"
                 % (len(bad), "y" if len(bad) == 1 else "ies"))
    out = make_blob(recs)
    open(a.out, "wb").write(out)
    body = len(out) - HDR
    print("packed %d messages -> %s (%d of %d bytes, %d spare)"
          % (len(entries), a.out, body, TARGET_BUDGET, TARGET_BUDGET - body))


def cmd_import(a):
    """Lift the text straight out of a donor ROM and target the US image."""
    donor = detect_region(a.donor)
    if donor == TARGET:
        print("note: donor is the US ROM itself -- this makes an identity blob")
    blob = load_block(a.donor, donor)
    recs = split_records(blob)

    # Briefings: decode with the DONOR's bases, re-encode with ours. A donor
    # tilemap cannot be copied as-is -- de/fr sit $10 above the US bases, so
    # the same tile number is a different letter.
    briefs = []
    if a.briefings:
        us_pk = brief_packets(a.us_rom, TARGET)
        dn_pk = brief_packets(a.donor, donor)
        for (src, us_data), (_, dn_data) in zip(us_pk, dn_pk):
            rows, _ = brief_to_rows(dn_data, BRIEF_BASES[donor],
                                    BRIEF_SPACE_ALIAS.get(donor))
            _, us_words = brief_to_rows(us_data, BRIEF_BASES[TARGET])
            briefs.append((src, brief_from_rows(rows, us_words, BRIEF_BASES[TARGET])))

    tiles = None
    if a.tiles_from:
        tiles = open(a.tiles_from, "rb").read()
        if len(tiles) != TILESCEN_RAW:
            sys.exit("%s is %d bytes; the scenario tileset is %d"
                     % (a.tiles_from, len(tiles), TILESCEN_RAW))
    elif a.tiles:
        # Take the donor's ARTWORK but keep our own glyphs.
        #
        # Briefings draw their letters out of this very tileset, and the two
        # regions put the alphabet in different places -- masked, US letters
        # begin at $290 and German at $2a1. Copying the tileset wholesale
        # therefore shifts every letter by $11 and turns the briefing text to
        # gibberish, which is exactly what it did in play. The cards and the
        # HUD strips live outside that block, so excluding it keeps the words
        # readable and still brings the translated pictures across.
        dn, _, _ = scen_tiles(a.donor, donor)
        us_raw, _, _ = scen_tiles(a.us_rom, TARGET)
        up = BRIEF_BASES[TARGET] & 0x3ff
        lo = up + 0x30
        # ONLY the slots the briefing text actually draws from. Preserving the
        # whole span between the two regions' bases also preserved the card
        # word-strips, which sit inside it -- the cards came back in English.
        keep = (set(range(up, up + 26)) |            # A-Z
                set(range(up + 0x1c, up + 0x1f)) |   # , . '
                set(range(up + 0x20, up + 0x2a)) |   # 0-9
                set(range(lo, lo + 26)))             # a-z
        tiles = bytearray(dn)
        for t in keep:
            tiles[t * 16:(t + 1) * 16] = us_raw[t * 16:(t + 1) * 16]
        tiles = bytes(tiles)
        print("  keeping %d US glyph tiles (%d..%d, discontinuous), donor art elsewhere"
              % (len(keep), min(keep), max(keep)))

    glyphs = accent_glyphs(a.donor, donor, a.us_rom) if not a.no_glyphs else []
    out = make_blob(recs, briefs, tiles, glyphs)
    open(a.out, "wb").write(out)
    body = len(recs and pack_records(recs))
    print("imported %s text from %s" % (donor, os.path.basename(a.donor)))
    print("  %d messages, %d bytes" % (len(recs), body))
    if briefs:
        print("  %d scenario briefings re-encoded to the US tile bases" % len(briefs))
    if glyphs:
        print("  %d accent glyphs into free US font slots" % len(glyphs))
    if tiles:
        print("  scenario picture tiles: %d tiles (%d bytes)"
              % (len(tiles) // 16, len(tiles)))
    print("  -> %s (%d bytes total)" % (a.out, len(out)))
    print("  US image has room for %d, so %d spare" % (TARGET_BUDGET, TARGET_BUDGET - body))
    print("")
    print("Run it with:  SC_TRANSLATION=%s" % a.out)


def cmd_glyphs(a):
    blob = load_block(a.rom, a.version)
    seen = sorted(set(blob) - {SEP})
    print("characters this region's text actually uses (%d):" % len(seen))
    line = ""
    for b in seen:
        ch = bytes([b]).decode(CODEC)
        line += ch
    print("  " + line)
    print("\nnon-ASCII among them:")
    for b in seen:
        if b >= 128:
            print("  $%02X  %s" % (b, bytes([b]).decode(CODEC)))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export"); e.set_defaults(fn=cmd_export)
    e.add_argument("--rom", required=True); e.add_argument("--version", required=True)
    e.add_argument("--out", required=True)
    k = sub.add_parser("pack"); k.set_defaults(fn=cmd_pack)
    k.add_argument("--in", required=True); k.add_argument("--out", required=True)
    i = sub.add_parser("import", help="donor ROM -> ready-to-run blob (one step)")
    i.set_defaults(fn=cmd_import)
    i.add_argument("--donor", required=True, help="a German/French/European ROM")
    i.add_argument("--out", required=True)
    i.add_argument("--us-rom", default="Sim City (U) [!].sfc",
                   help="the US image the blob targets (for briefing layout)")
    # OFF by default: the packet INDEX does not mean the same screen in every
    # region. Measured exactly -- the live German "Advice" screen is German
    # packet 6, while US packet 6 is the free-play welcome block Sylt borrows.
    # Pairing by index therefore lays one screen's text over another's layout,
    # which is the gibberish this produced in play. Pair them by identity
    # first; until then messages only.
    i.add_argument("--no-glyphs", action="store_true",
                   help="skip the accent glyphs (umlauts and accents)")
    i.add_argument("--tiles", action="store_true",
                   help="also take the donor's scenario picture tiles "
                        "(the cards and HUD word-strips)")
    i.add_argument("--tiles-from", metavar="BIN",
                   help="use an edited scenario tileset .bin instead of the donor's")
    i.add_argument("--briefings", action="store_true",
                   help="also substitute scenario briefings (BROKEN: packets "
                        "are paired by index, which is wrong across regions)")
    tl = sub.add_parser("tiles", help="export the scenario picture tiles")
    tl.set_defaults(fn=cmd_tiles)
    tl.add_argument("--rom", required=True); tl.add_argument("--out", required=True)
    g = sub.add_parser("glyphs"); g.set_defaults(fn=cmd_glyphs)
    g.add_argument("--rom", required=True); g.add_argument("--version", required=True)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
