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


def scen_glyphs(donor_rom, donor):
    """(tile index, 16 raw bytes) for the accented briefing glyphs."""
    raw, _, _ = scen_tiles(donor_rom, donor)
    out = []
    for src_t, dst_t in BRIEF_EXTRA_COPY.items():
        g = raw[(src_t & 0x3ff) * 16:((src_t & 0x3ff) + 1) * 16]
        if g and g != bytes(16):
            out.append((dst_t & 0x3ff, g))
    return out


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


# ── briefings as STRINGS (the Sylt model) ────────────────────────────────
# sylt_write_brief_tilemap() does not transplant a tilemap: it clears the page
# and composes the text with brief_put(), title on row 2 col 5 and body from
# row 4 col 4. Carrying strings and composing them the same way is what makes
# a briefing translatable, and it sidesteps every donor-tilemap problem --
# bases, space aliases, punctuation layout, packet pairing -- because the US
# side does the drawing.
#
# The two base pairs are the colour: the title is drawn from a different glyph
# bank than the body, which is why it is a different colour on screen.
BRIEF_TITLE_BASE = 0x000        # title bank: up=$000, lo=$030
BRIEF_BODY_BASE = 0x690         # body bank:  up=$690, lo=$6c0
BRIEF_TITLE_ROW, BRIEF_TITLE_COL = 2, 5
BRIEF_BODY_ROW, BRIEF_BODY_COL = 4, 4
BRIEF_ROWS = 64
# The paper is 32 columns wide. Text past it is simply not drawn by brief_put,
# so a long line is silently truncated rather than corrupting anything -- but
# it still loses words, so the tool refuses instead.
BRIEF_TITLE_MAX = BRIEF_COLS - BRIEF_TITLE_COL      # 27
BRIEF_BODY_MAX = BRIEF_COLS - BRIEF_BODY_COL        # 28


# (uppercase base, lowercase base) per region. NOT "lo = up + 0x30": that is
# the US layout, and assuming it everywhere put German capitals 16 slots out.
# Reported from play as "2imSity" for SimCity and "Uin starkes Urdbeben" --
# every wrong character a capital, every one off by exactly +16, lower case
# untouched. Working back: German S rendered as the digit 2, i.e. up+0x20+2,
# so German S sits at $6c3 and its upper-case bank starts at $6b1.
#
# The gap differs because the glyphs between the two banks differ per region.
# It also means the US digit offset (up+0x20) lands ON German's lower case, so
# digits are only decoded where that does not collide.
BRIEF_BANKS = {"us": (0x690, 0x6c0), "eu": (0x690, 0x6c0),
               "de": (0x6b1, 0x6d1), "fr": (0x6b1, 0x6d1)}

# Glyphs outside the two letter banks, identified from the German words they
# sit inside -- reported from play as B*rgermeister, H*re/l*se, Sch*den, mu*t,
# Krimi*nalit*t. Two independent words agree on the o-umlaut slot, which is
# the cross-check. Frequency alone could not tell these apart and bitmap
# comparison against the dialog font fails: the briefing bank is different
# artwork.
#
# The four umlaut slots are unused by the US briefings AND blank in the US
# tileset, so the donor's glyphs drop in at the same tile numbers. The hyphen
# is already drawn on the US side and only needed mapping.
# German keeps its punctuation and digits in their own region BELOW the
# uppercase bank, not at the US up+$1C..$2A offsets. Digits were read off two
# lines that say "10" and "5" before the word for years -- $6a1,$6a0 and $6a5
# -- which fixes the digit base at $6a0 and checks itself.
BRIEF_EXTRA = {0x69c: ",", 0x69d: "-", 0x69e: ".",
               0x6f1: "ü", 0x6f4: "ä", 0x704: "ö", 0x70b: "ß",
               0x6f2: "é", 0x6fa: "è", 0x697: "'",
               0x6f3: "â", 0x6fb: "ï"}
# French shares German's arrangement -- MEASURED, not assumed: it uses the same
# comma/hyphen/period slots and the same $6a0 digit base, and its lower case
# letters rank e s t n i a r o, which is French's own frequency order.
BRIEF_DIGITS = {"de": 0x6a0, "fr": 0x6a0}

# donor tile -> the US tile it is copied to. The umlauts keep their numbers
# because those are blank on the US side. The HYPHEN cannot: $69d on the US
# side is up+13, the letter N -- which is exactly what it drew. It gets a free
# slot instead.
# donor tile -> the US tile it is copied to. The umlauts keep their numbers
# because those are blank on the US side. The HYPHEN cannot: $69d on the US
# side is up+13, the letter N -- which is exactly what it drew.
#
# Its slot is $6ec rather than $6f5: French USES $6f5 for an accent of its own,
# so a hyphen parked there would collide the moment French is translated.
# $6ec is blank in the US tileset and untouched by all three regions.
BRIEF_EXTRA_COPY = {0x6f1: 0x6f1, 0x6f4: 0x6f4, 0x704: 0x704, 0x70b: 0x70b,
                    0x69d: 0x6ec, 0x6f2: 0x6ed, 0x6fa: 0x6ee,
                    0x6f3: 0x6ef, 0x6fb: 0x6f0}
# The apostrophe ($697) needs no copy: the US already draws one at up+$1E,
# and brief_put maps it there.
BRIEF_HYPHEN_US = 0x6ec


def _dec_bank(t, up, lo=None, digits=None):
    if t in BRIEF_EXTRA:
        return BRIEF_EXTRA[t]
    if digits is not None and digits <= t < digits + 10:
        return chr(ord("0") + t - digits)
    if lo is None:
        lo = up + 0x30
    # Lower case first: its base is the one derived from letter frequencies
    # and confirmed on screen, so it wins any overlap with the digit range.
    if lo <= t < lo + 26:          return chr(ord("a") + t - lo)
    if up <= t < up + 26:          return chr(ord("A") + t - up)
    if up + 0x20 + 10 <= lo or up + 0x2a <= lo:
        if up + 0x20 <= t < up + 0x2a: return chr(ord("0") + t - up - 0x20)
    if t == up + 0x1c:             return ","
    if t == up + 0x1d:             return "."
    if t == up + 0x1e:             return "'"
    return None


def brief_rows_text(data, title_base, body_base, space_alias=None,
                    body_lo=None, title_lo=None, digits=None):
    """(title, [body lines]) read out of one decompressed briefing page."""
    import struct
    w = struct.unpack("<%dH" % (len(data) // 2), data[:len(data) // 2 * 2])
    rows = []
    for r in range(0, min(len(w) // BRIEF_COLS, BRIEF_ROWS)):
        cells = w[r * BRIEF_COLS:(r + 1) * BRIEF_COLS]
        line = ""
        for t in cells:
            c = _dec_bank(t, body_base, body_lo, digits)
            if c is None:
                c = _dec_bank(t, title_base, title_lo)
            if c is None:
                c = " " if (t == BRIEF_BLANK or t == space_alias) else " "
            line += c
        rows.append(line.rstrip())
    title = rows[BRIEF_TITLE_ROW].strip() if len(rows) > BRIEF_TITLE_ROW else ""

    # Take each page's OWN origin rather than forcing row 4 / column 4. The
    # pages do not share one layout: composing them all at the same place put
    # text "somewhere in the middle of the textfield", reported from play.
    first = None
    left = BRIEF_COLS
    for r in range(BRIEF_TITLE_ROW + 1, len(rows)):
        stripped = rows[r].rstrip()
        if not stripped:
            continue
        if first is None:
            first = r
        left = min(left, len(rows[r]) - len(rows[r].lstrip()))
    if first is None:
        return title, [], BRIEF_BODY_ROW, BRIEF_BODY_COL
    body = [r[left:].rstrip() for r in rows[first:]]
    while body and not body[-1]:
        body.pop()
    return title, body, first, left


def detect_region(rom_path):
    rom = open(rom_path, "rb").read()
    if len(rom) < 0x8000:
        sys.exit("%s is too small to be a SimCity ROM" % rom_path)
    b = rom[0x7fd9]
    if b not in REGION_BYTE:
        sys.exit("%s: region byte $%02X is not one this tool knows "
                 "(Japan stores text as tile indices, not characters)" % (rom_path, b))
    return REGION_BYTE[b]


def make_blob(records, briefs=(), tiles=None, glyphs=(), strings=None,
              sglyphs=(), cards=None, surfaces=None):
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
    out = (MAGIC + bytes([9, 0x01])
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
    out += (len(strings) if strings else 0).to_bytes(4, "little")
    if strings:
        out += strings
    out += len(sglyphs).to_bytes(2, "little")
    for idx, g in sglyphs:
        out += idx.to_bytes(2, "little") + g
    out += (len(cards) if cards else 0).to_bytes(4, "little")
    if cards:
        out += cards
    out += (len(surfaces) if surfaces else 0).to_bytes(4, "little")
    if surfaces:
        out += surfaces
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

    # Briefings as STRINGS, composed by the US build. Words come from the
    # donor, addresses from the US image, because the blob is recognised by
    # the address the US build decompresses.
    strings = None
    if a.briefs:
        import io, contextlib
        us_doc = _briefs_doc(a.us_rom)
        dn_doc = _briefs_doc(a.donor)
        by_page = {q["page"]: q for q in dn_doc["pages"]}
        pages, bad = [], []
        for q in us_doc["pages"]:
            o = by_page.get(q["page"])
            title = (o or q)["title"]
            body = (o or q)["body"]
            if len(title) > BRIEF_TITLE_MAX:
                bad.append("page %d title %d > %d" % (q["page"], len(title), BRIEF_TITLE_MAX))
            for line in body:
                if len(line) > BRIEF_BODY_MAX:
                    bad.append("page %d line %d > %d" % (q["page"], len(line), BRIEF_BODY_MAX))
            pages.append((q["src"], title, body,
                          o.get("row", BRIEF_BODY_ROW) if o else q.get("row", BRIEF_BODY_ROW),
                          o.get("col", BRIEF_BODY_COL) if o else q.get("col", BRIEF_BODY_COL)))
        if bad:
            for m in bad[:10]:
                print("  " + m, file=sys.stderr)
            sys.exit("%d briefing line(s) run off the paper" % len(bad))
        buf = bytearray(len(pages).to_bytes(2, "little"))
        for src, title, body, brow, bcol in pages:
            buf += src.to_bytes(4, "little")
            buf += bytes([brow & 0xff, bcol & 0xff])
            tb = title.encode("latin-1", "replace")[:BRIEF_TITLE_MAX]
            buf += bytes([len(tb)]) + tb
            buf += bytes([min(len(body), 255)])
            for line in body[:255]:
                lb = line.encode("latin-1", "replace")[:BRIEF_BODY_MAX]
                buf += bytes([len(lb)]) + lb
        strings = bytes(buf)

    sglyphs = scen_glyphs(a.donor, donor) if a.briefs else []

    # Scenario cards, from a selector VRAM capture of the donor. Column 42 is
    # skipped: that is the ninth slot, drawn host-side by sylt_place_card(),
    # and importing the donor's empty slot over it would erase Sylt's card.
    cards = None
    if a.cards_from:
        buf = bytearray()
        n = 0
        for f in sorted(os.listdir(a.cards_from)):
            if not f.startswith("scenario_card_") or not f.endswith(".bin"):
                continue
            d = open(os.path.join(a.cards_from, f), "rb").read()
            if len(d) < 11 or d[:4] != CARD_MAGIC:
                sys.exit("%s is not a card file" % f)
            if d[7] == 42:
                # Sylt's slot: take its TILES but not its layout. Card names
                # are letters, and letters are shared -- part of the donor's
                # alphabet is referenced only from this column, so skipping it
                # entirely left some glyphs English and the names mixed. h=0
                # marks the entry "art only", and sylt_place_card() redraws
                # its own card afterwards regardless.
                # Art only: zero the height AND drop the tilemap bytes it
                # describes, or the parser reads the tile data 144 bytes early
                # and abandons every record after this one.
                w0, h0 = d[5], d[6]
                head = d[:6] + bytes([0]) + d[7:11]
                d = head + d[11 + w0 * h0 * 2:]
            buf += d
            n += 1
        if n:
            cards = n.to_bytes(2, "little") + bytes(buf)
    surfs = b""
    for f in (a.surface or []):
        d = open(f, "rb").read()
        if d[:4] != SURF_MAGIC:
            sys.exit("%s is not a surface file" % f)
        surfs += len(d).to_bytes(4, "little") + d
    if surfs:
        surfs = len(a.surface).to_bytes(2, "little") + surfs

    out = make_blob(recs, briefs, tiles, glyphs, strings, sglyphs, cards,
                    surfs or None)
    open(a.out, "wb").write(out)
    body = len(recs and pack_records(recs))
    print("imported %s text from %s" % (donor, os.path.basename(a.donor)))
    print("  %d messages, %d bytes" % (len(recs), body))
    if briefs:
        print("  %d scenario briefings re-encoded to the US tile bases" % len(briefs))
    if glyphs:
        print("  %d accent glyphs into free US font slots" % len(glyphs))
    if strings:
        print("  %d briefing pages as strings, composed US-side" % len(pages))
    if sglyphs:
        print("  %d accented briefing glyphs into free US tileset slots" % len(sglyphs))
    if surfs:
        print("  %d surface(s)" % len(a.surface))
    if cards:
        print("  %d scenario cards (Sylt's slot left alone)"
              % int.from_bytes(cards[:2], "little"))
    if tiles:
        print("  scenario picture tiles: %d tiles (%d bytes)"
              % (len(tiles) // 16, len(tiles)))
    print("  -> %s (%d bytes total)" % (a.out, len(out)))
    print("  US image has room for %d, so %d spare" % (TARGET_BUDGET, TARGET_BUDGET - body))
    print("")
    print("Run it with:  SC_TRANSLATION=%s" % a.out)


def _briefs_doc(rom_path):
    ver = detect_region(rom_path)
    tb = BRIEF_TITLE_BASE
    bb, blo = BRIEF_BANKS[ver]
    pages = []
    for i, (src, d) in enumerate(brief_packets(rom_path, ver)):
        title, body, brow, bcol = brief_rows_text(
            d, tb, bb, BRIEF_SPACE_ALIAS.get(ver), blo,
            digits=BRIEF_DIGITS.get(ver))
        pages.append({"page": i, "src": src, "title": title, "body": body,
                      "row": brow, "col": bcol})
    return {"version": ver, "pages": pages}


def cmd_briefs(a):
    """Export every briefing page as editable strings."""
    ver = detect_region(a.rom)
    tb = BRIEF_TITLE_BASE
    bb, blo = BRIEF_BANKS[ver]
    pages = []
    for i, (src, d) in enumerate(brief_packets(a.rom, ver)):
        title, body, brow, bcol = brief_rows_text(
            d, tb, bb, BRIEF_SPACE_ALIAS.get(ver), blo,
            digits=BRIEF_DIGITS.get(ver))
        pages.append({"page": i, "src": src, "title": title, "body": body,
                      "row": brow, "col": bcol})
    doc = {
        "_readme": [
            "One entry per briefing page: the tutorial, the scenarios and the",
            "ninth. Translate \"title\" and the \"body\" lines; leave \"page\" and",
            "\"src\" alone -- src is the ROM address the page is recognised by.",
            "The paper is %d columns wide: a title may be %d characters and a"
            % (BRIEF_COLS, BRIEF_TITLE_MAX),
            "body line %d. Longer lines are refused rather than cut off."
            % BRIEF_BODY_MAX,
            "The title is drawn from a different glyph bank than the body, which",
            "is what makes it a different colour. That is handled for you.",
            "Only A-Z a-z 0-9 , . ' and space have glyphs here.",
        ],
        "version": ver,
        "pages": pages,
    }
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
    nb = sum(len(p["body"]) for p in pages)
    print("exported %d briefing pages (%d body lines) -> %s"
          % (len(pages), nb, a.out))
    over = [(p["page"], len(p["title"]), max([len(x) for x in p["body"]] or [0]))
            for p in pages
            if len(p["title"]) > BRIEF_TITLE_MAX
            or any(len(x) > BRIEF_BODY_MAX for x in p["body"])]
    if over:
        print("  NOTE: %d page(s) already exceed the paper: %s" % (len(over), over))


def cmd_briefs_pack(a):
    """Edited briefing strings -> a blob the US build composes itself."""
    doc = json.load(open(getattr(a, "in"), encoding="utf-8"))
    # --text-from takes the WORDS from another region's export while keeping
    # THIS one's src addresses. The blob is recognised by the address the US
    # build decompresses, and a donor export carries the donor's addresses,
    # which the US build never asks for. Paired by page index: the furniture
    # tiles agree on the diagonal for 10 of 12 pages, with clear margins.
    if a.text_from:
        other = json.load(open(a.text_from, encoding="utf-8"))
        src_pages = {q["page"]: q for q in other["pages"]}
        for q in doc["pages"]:
            o = src_pages.get(q["page"])
            if o:
                q["title"] = o["title"]
                q["body"] = o["body"]
        print("  words from %s (%s), addresses from %s"
              % (os.path.basename(a.text_from), other.get("version", "?"),
                 doc.get("version", "?")))
    pages, bad = [], []
    for p in doc["pages"]:
        title = p.get("title", "")
        body = [x for x in p.get("body", [])]
        if len(title) > BRIEF_TITLE_MAX:
            bad.append("page %d title is %d chars, the paper allows %d"
                       % (p["page"], len(title), BRIEF_TITLE_MAX))
        for k, line in enumerate(body):
            if len(line) > BRIEF_BODY_MAX:
                bad.append("page %d line %d is %d chars, the paper allows %d"
                           % (p["page"], k, len(line), BRIEF_BODY_MAX))
        if BRIEF_BODY_ROW + len(body) > BRIEF_ROWS:
            bad.append("page %d has %d body lines, the page holds %d"
                       % (p["page"], len(body), BRIEF_ROWS - BRIEF_BODY_ROW))
        pages.append((p["src"], title, body))
    if bad:
        for m in bad[:12]:
            print("  " + m, file=sys.stderr)
        sys.exit("%d line(s) would run off the paper; shorten them" % len(bad))

    out = bytearray()
    out += len(pages).to_bytes(2, "little")
    for src, title, body in pages:
        out += src.to_bytes(4, "little")
        tb = title.encode("latin-1", "replace")[:BRIEF_TITLE_MAX]
        out += bytes([len(tb)]) + tb
        out += bytes([min(len(body), 255)])
        for line in body[:255]:
            lb = line.encode("latin-1", "replace")[:BRIEF_BODY_MAX]
            out += bytes([len(lb)]) + lb
    blob = make_blob([b""], (), None, (), bytes(out))
    open(a.out, "wb").write(blob)
    nl = sum(len(b) for _, _, b in pages)
    print("packed %d briefing pages (%d body lines) -> %s"
          % (len(pages), nl, a.out))
    print("  the US build composes these itself, the way Sylt's briefing is drawn")


# ── scenario cards ───────────────────────────────────────────────────────
# The selector lays its cards out on a 5x2 grid, found from the drop-shadow
# tile Sylt also uses ($010): vertical runs mark each card's right edge at
# columns 10/20/30/40/50, horizontal runs its bottom at rows 14 and 25. So a
# card is 8x9 tiles with its top-left at one of columns 2/12/22/32/42 and rows
# 5/16 -- and Sylt's own card sits at (42, 5), exactly in that grid, which is
# what confirms the geometry.
#
# Unlike Sylt's, the shipped cards are NOT a consecutive tile run: they are
# ordinary tilemap rectangles over shared tiles. So a card file has to carry
# both the 8x9 tilemap AND the artwork of every tile it references.
CARD_COLS = [2, 12, 22, 32, 42]
CARD_ROWS = [5, 16]
CARD_W, CARD_H = 8, 9
CARD_MAGIC = b"SCCD"
BG1_CHR_WORDS = 16          # 4bpp: 32 bytes = 16 VRAM words a tile


def _vram_words(path):
    import struct
    d = open(path, "rb").read()
    return struct.unpack("<%dH" % (len(d) // 2), d[:len(d) // 2 * 2])


def card_extract(w, mapbase, chrbase, col, row):
    """(tilemap entries, {tile index: 32 raw bytes}) for one card slot."""
    import struct
    ents = []
    for y in range(CARD_H):
        for x in range(CARD_W):
            c = col + x
            page = 0x400 if c >= 32 else 0
            ents.append(w[mapbase + page + (row + y) * 32 + (c & 31)])
    art = {}
    for e in ents:
        t = e & 0x3ff
        if t in art:
            continue
        base = chrbase + t * BG1_CHR_WORDS
        art[t] = struct.pack("<%dH" % BG1_CHR_WORDS,
                             *w[base:base + BG1_CHR_WORDS])
    return ents, art


def cmd_cards(a):
    import struct
    w = _vram_words(a.vram)
    os.makedirs(a.out, exist_ok=True)
    n = 0
    for ri, row in enumerate(CARD_ROWS):
        for ci, col in enumerate(CARD_COLS):
            ents, art = card_extract(w, a.map, a.chr, col, row)
            blob = bytearray(CARD_MAGIC + bytes([1, CARD_W, CARD_H]))
            blob += bytes([col, row])
            blob += len(art).to_bytes(2, "little")
            for e in ents:
                blob += e.to_bytes(2, "little")
            for t in sorted(art):
                blob += t.to_bytes(2, "little") + art[t]
            name = os.path.join(a.out, "scenario_card_r%dc%02d.bin" % (ri, col))
            open(name, "wb").write(blob)
            n += 1
            print("  %-34s %2d tiles, %d bytes"
                  % (os.path.basename(name), len(art), len(blob)))
    print("exported %d cards from %s" % (n, os.path.basename(a.vram)))


# ── surfaces ─────────────────────────────────────────────────────────────
# A surface is the general form of what sylt_place_card() does for one card:
# a rectangle of tilemap plus the artwork of every tile it references, written
# into VRAM when a given screen appears. Cards were the first instance; the
# menus, the title's prompt and the HUD labels are the same shape.
#
# Tiles are NOT written at the donor's own indices. On the selector BG3 shares
# CHR space with BG1 at $0000, so a donor tile placed at its own index would
# land on the card pictures. Indices are allocated from what the TARGET screen
# leaves free, and the tilemap is repointed to match.
SURF_MAGIC = b"SCSF"


def _info(dump):
    d = {}
    try:
        for line in open(dump + ".info", encoding="utf-8"):
            k, _, v = line.strip().partition("=")
            if k:
                d[k] = v
    except OSError:
        sys.exit("%s.info is missing -- retake the capture with a build that "
                 "writes the sidecar" % dump)
    return d


def _depth_words(mode, layer):
    """VRAM words per tile: 4bpp is 16, 2bpp is 8.

    Keyed on the LAYER, not the reported mode. The selector's sidecar says
    bgmode=0, in which every background is nominally 2bpp -- but the card
    pictures on BG1 only read correctly at 16 words a tile, so the reported
    mode is not what the layer is actually fetched at here. BG3 carries the
    text and is 2bpp in every mode this game uses.
    """
    return 8 if layer in (3, 4) else 16


def _parse_rows(spec):
    out = []
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-")
            out.extend(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    return out


def _used_tiles(w, info, rows_all=range(32)):
    """every tile index any layer of this screen references"""
    used = set()
    for L in (1, 2, 3, 4):
        key = "bg%dmap" % L
        if key not in info:
            continue
        m = int(info[key], 16)
        for r in rows_all:
            for c in range(64):
                page = 0x400 if c >= 32 else 0
                used.add(w[m + page + r * 32 + (c & 31)] & 0x3ff)
    return used


def cmd_surface(a):
    import struct
    din, tin = _info(a.donor), _info(a.target)
    if din.get("screen") != tin.get("screen"):
        sys.exit("captures are of different screens: donor $%s, target $%s"
                 % (din.get("screen"), tin.get("screen")))
    dw, tw = _vram_words(a.donor), _vram_words(a.target)
    dmap = int(din["bg%dmap" % a.layer], 16)
    dchr = int(din["bg%dchr" % a.layer], 16)
    tmap = int(tin["bg%dmap" % a.layer], 16)
    nw = _depth_words(din.get("bgmode", 1), a.layer)
    rows = _parse_rows(a.rows)
    # Columns matter as much as rows. Writing the full 64-column width of a
    # row replaces the BACKGROUND too, and the donor's background is not the
    # target's -- the German build runs none of the host margin work, so
    # copying its full rows wiped the wood. Reported from play as cards and
    # background completely broken.
    cols = _parse_rows(a.cols) if a.cols else list(range(64))

    ents = []
    for r in rows:
        for c in cols:
            page = 0x400 if c >= 32 else 0
            ents.append(dw[dmap + page + r * 32 + (c & 31)])

    # A slot is free only if its CHR is EMPTY in the target -- not merely
    # unreferenced by the captured tilemap.
    #
    # The capture is one frame at one scroll position. Cards that were off
    # screen then reference tiles that look unused, so allocating over them
    # destroyed every card the capture had not been showing. Reported from
    # play as all cards broken except Sylt, which is the one this never
    # writes to. Emptiness in CHR is a property of the image, not of what a
    # single frame happened to draw.
    tchr = int(tin["bg%dchr" % a.layer], 16)
    taken = _used_tiles(tw, tin)
    free = []
    for t in range(1, 1024):
        if t in taken:
            continue
        base = tchr + t * nw
        if base + nw > 0x8000:
            break
        if any(tw[base:base + nw]):
            continue                     # something is drawn here already
        free.append(t)
    remap, art = {}, []
    for e in ents:
        t = e & 0x3ff
        if t in remap:
            continue
        base = dchr + t * nw
        px = struct.pack("<%dH" % nw, *dw[base:base + nw])
        if px == bytes(nw * 2):
            remap[t] = t            # blank: leave it alone
            continue
        if not free:
            sys.exit("no free tile slots left on the target screen")
        remap[t] = free.pop(0)
        art.append((remap[t], px))

    blob = bytearray(SURF_MAGIC + bytes([1, a.layer, int(din["screen"], 16)]))
    blob += tmap.to_bytes(2, "little") + nw.to_bytes(1, "little")
    blob += len(rows).to_bytes(1, "little")
    for r in rows:
        blob += bytes([r])
    blob += len(cols).to_bytes(1, "little")
    for c in cols:
        blob += bytes([c])
    blob += len(art).to_bytes(2, "little")
    for e in ents:
        blob += (((e & ~0x3ff) | remap[e & 0x3ff]) & 0xffff).to_bytes(2, "little")
    for idx, px in art:
        blob += idx.to_bytes(2, "little") + px
    open(a.out, "wb").write(blob)
    print("surface: screen $%s layer BG%d, %d rows x %d cols, %d tiles remapped"
          % (din["screen"], a.layer, len(rows), len(cols), len(art)))
    print("  %d bytes -> %s" % (len(blob), a.out))


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



# ── screen packets: patching a screen at its own ROM source ──────────────
# The scenario selector is not assembled at runtime. The ROM keeps its whole
# 64x32 tilemap as one compressed packet and the card artwork as another, and
# unpacks both through the routine at 00:90DD. Every earlier attempt wrote the
# translated cards straight into VRAM from the host and lost a race with the
# game's own NMI DMA -- names came out fragmented, one card's text running
# into the next. Patching the DECOMPRESSED PACKET instead means the game
# uploads the translated screen itself, on its own schedule, so there is no
# race left to lose.
#
# The two packets were found by decompressing every LZ5 stream in the US and
# German ROMs and matching them: 05A5A1 is the only 4096-byte packet outside
# the briefing group whose content differs between the regions, and it agrees
# 82.7% with the live selector tilemap at VRAM $3000 (the rest is this
# project's own Sylt card and margin work). 0444DB lands at VRAM $0000, the
# selector's BG1 CHR base.
PACKET_MAGIC = b"SCPK"
PACKET_HDR = 8

# Anchors are US file offsets. The donor's copy of each is found by content,
# not by a hardcoded table, so a French or European ROM needs no new numbers.
SELECTOR_MAP = 0x05A5A1
SELECTOR_CHR = 0x0444DB
# The card name strips are 2bpp, 16 bytes a tile, and they start at VRAM byte
# $0B80 -- tile 184. Confirmed against a live capture: read at that stride the
# block spells "San Francisco Earthquake Bern Traffic Detroit Crime Tokyo
# Monster Attack ... MAP SELECT", and the German ROM's copy of the same block
# spells the German equivalents at the same address.
#
# This is where the first version of this tool was wrong. It read the artwork
# at 32 bytes a tile because the card PICTURES on this screen are 4bpp, so it
# copied two tiles' worth of bytes for every glyph and landed them at double
# the offset. The tile indices it derived were right; the stride was not.
# The names are whole pre-rendered WORDS, not letters -- a strip only reads
# correctly as an unbroken run, which is why a half-right copy came out as
# fragments running between cards rather than as wrong letters.
CHR_BYTES_PER_TILE = 16          # 2bpp
CHR_TILES = 16384 // CHR_BYTES_PER_TILE          # the packet holds 1024

# Where the shipped layout keeps the flooding strip (Rio's card), and where
# it goes on Sylt's 8x9 card. Rows 6 and 7 are the two drawn lines
# ("Coastal" / "Flooding"); the host blanks both and this lands on row 7,
# directly above the year, the way every shipped card sits.
SYLT_SRC_ROW, SYLT_SRC_COL, SYLT_STRIP_LEN = 11, 23, 6
SYLT_CARD_ROW, SYLT_CARD_COL, SYLT_CARD_W = 7, 1, 8
SYLT_CARD_PSEUDO = -1            # not a ROM packet; encoded as $ff:ffff

# The main map's building labels are NOT in a packet. They sit uncompressed
# at file $034C00 and are copied to VRAM byte $CC00 one for one -- found by
# taking tiles straight out of a live capture and searching the ROM for them:
# 70 of 80 matched consecutively from that base, and none of them appeared in
# any compressed packet. The German ROM keeps its own labels at the SAME
# address with the same 16-tile rows, so the donor's bytes drop straight in.
ROM_SPAN_PSEUDO = -2             # a raw cart-image span; encoded as $fe:ffff
HUD_LABELS = (0x034C00, 0x0376C0)

# The artwork is only half of a label. Where each tile GOES -- which row,
# what x, and whether the label is one centred line or two -- lives in a
# 60-byte record per building, reached through a pointer table at $01:8FC4
# indexed by the tool in $020d. The routine at 01:8F25 walks the record
# writing (position, tile) word pairs into the sprite staging buffer.
#
# Found by watching the writes rather than reading the ROM: six searches for
# a start/length table and five over the generated C all missed, because
# there is no such table -- every sprite carries its own position.
#
# Copying the donor's artwork WITHOUT these records is what produced
# "n-en lei / tung Parkal" in play: German pixels cut at English positions.
# The pointer table is identical across regions, so the records drop in.
HUD_RECORDS = (0x008FE4, 0x0093A4)      # 16 records x 60 bytes

# -- the main menu -------------------------------------------------------
# The menu's words are SPRITES, emitted by 00:8EA9 from a record chosen by
# $0261 through a pointer table at $00:A164 (docs/ROM_MAP.md). A record is a
# flags word then up to eight (X, Y, tile+attr) sprites -- 34 bytes at most.
#
# $00:A164 is the GENERAL sprite-text table: it serves the title and every
# other screen too. Copying its whole region from a donor, as 4228a5f did,
# replaces the TITLE's records with German ones that index artwork the title
# never loads, and the title comes up frozen with corrupt tiles. So only the
# menu's own four entries are touched, and their records are RELOCATED into
# filler rather than written over the US records that other screens use.
MENU_INDICES = (0x0C, 0x0D, 0x0F, 0x10)
MENU_TABLE = 0x002164                    # FILE offset of $00:A164 (addr - $8000)
MENU_FREE = 0x007B4C                     # 1140 bytes of $FF, ending at the header
MENU_FREE_END = 0x007FC0                 # the cartridge header starts here
MENU_ART = 0x04A571                      # the artwork the records index into

# The menu font is 8 wide x 16 tall: two stacked 8x8 tiles per character, and
# a 16x16 sprite carries TWO characters side by side. The artwork already
# holds the whole uppercase alphabet in that form -- rows 0-1 are A-P, rows
# 2-3 are Q-Z followed by ! ? - . -- and it is the SAME face the word strips
# use, not a lookalike: composing "RESUME" from these glyphs reproduces the
# RESUMESAVED strip at rows 4-5 byte for byte.
#
# So no glyph harvesting from words is needed. Any string can be composed by
# copying alphabet glyphs into free artwork tiles.
#
# (The 8x8 alphabet elsewhere in the sheet is a different, smaller face and
# will not match -- that was the wrong lead.)
MENU_FONT = {}
for _i, _ch in enumerate("ABCDEFGHIJKLMNOP"):
    MENU_FONT[_ch] = 0 * 16 + _i
for _i, _ch in enumerate("QRSTUVWXYZ!?-."):
    MENU_FONT[_ch] = 2 * 16 + _i


# Where composed text goes. Rows 30-31 of the artwork are blank AND never
# referenced by any sprite on the title or menu screens -- and this packet is
# only resident there, so nothing else can be looking at them. Established by
# snapshotting OAM on every screen that loads it and taking the union of the
# tiles actually displayed, NOT by scanning the record table: that scan called
# rows 2-3 free and composing there broke START NEW CITY, which draws tile
# $022 from a record the scan never reached.
# -- the menu generator --------------------------------------------------
# The three option lines are drawn by three consecutive 34/34/11-byte chunks
# at $00:A3CE, $00:A3F0 and $00:A412: a flags word, then 4-byte sprite entries
# (x, y, tile low, tile high + attribute), ending either on the emitter's
# eight-sprite budget or on a single x=$00 byte whose flag bit is set. Only
# the first is in the record table (index $0F); the other two are reached by
# the emitter carrying on past its budget, which is why chasing pointers to
# them never terminated -- $A3F0 and $A412 are pointed at by nothing.
#
# They were found by signature instead: each entry stores x and y as offsets
# from the caller's base (136, 116), so searching the record region for the
# three bytes of a sprite seen on screen locates its entry. Eighteen of the
# nineteen sprites on the option lines resolve uniquely and contiguously; the
# nineteenth is the cursor arrow, a separate record drawn from base (50, 112),
# and it is left alone.
#
# Because x, y and tile all live in the entry, those eighteen are a free pool:
# any of them can be given to any line. That is what lifts the eight-character
# cap. The US layout spends 4 + 7 + 7, but nothing fixes that split.
#
#   $A3CE  8 sprites   y=160 x131..179  then  y=112 x74..122
#   $A3F0  8 sprites   y=136 x74..170   then  y=160 x106
#   $A412  2 sprites   y=160 x74, x90
#
# Growing the pool past eighteen is NOT possible in place: the $A412 chunk
# ends at $A41C and record $10 begins at $A41D. It would take relocating $10
# into the filler at $00:FB4C, and $10 is the logo, which has broken this
# screen before. Eighteen sprites is thirty-six characters, which is enough.
MENU_SPRITES = [0xA3D0, 0xA3D4, 0xA3D8, 0xA3DC, 0xA3E0, 0xA3E4, 0xA3E8,
                0xA3EC, 0xA3F2, 0xA3F6, 0xA3FA, 0xA3FE, 0xA402, 0xA406,
                0xA40A, 0xA40E, 0xA414, 0xA418]
# The pool as the emitter sees it: record $0F is ONE record of three chunks.
# $00:8F4A, reached when the eight-sprite budget runs out, jumps back to
# $00:8EBA, which reloads the budget and reads a FRESH flags word from the
# next two bytes -- so a record simply carries on, 8 sprites at a time, until
# a chunk terminates. That is why nothing points at $A3F0 or $A412.
MENU_CHUNKS = [(0xA3CE, 8), (0xA3F0, 8), (0xA412, 2)]
MENU_BASE_X, MENU_BASE_Y = 136, 116      # $025d / $025f for these three lines
MENU_LINE_Y = (112, 136, 160)            # the arrow's stops, from table $d37c
MENU_LINE_X0 = 74                        # all three lines are left-aligned here

# Composed glyphs go in artwork rows 20-31, none of which is displayed on any
# screen that loads this packet -- established by snapshotting OAM on each of
# them and taking the union of the tiles actually used, NOT by scanning the
# record table. That scan called rows 2-3 free; composing there broke START
# NEW CITY, which draws tile $022 from a chunk the scan never reached.
# A band is a pair of rows: a character's top half sits in the first and its
# bottom half sixteen tiles later, in the second. Rows 30-31 are blank as well
# as unused, so they come first.
MENU_FREE_BANDS = (0x1E0, 0x1C0, 0x140, 0x160, 0x180, 0x1A0)
MENU_BAND_COLS = 16



# -- accents ---------------------------------------------------------------
# The font has no accented letters, and there is no room to add a sprite for
# the marks: the German cartridge draws the dots of UEBUNGSSPIEL as an extra
# 8x8 sprite at y=104, and this pool has no spare entry to spend on one.
#
# So the mark is composited into the character cell instead. The cell is 8x16
# and the letters fill all sixteen rows, but they are drawn as a vertical
# colour ramp, so two rows can come out of the middle without changing the
# shape: the squash picks the rows that differ least from the row above, which
# lands on the plain vertical strokes every time. The letter keeps its apex
# and its base, loses two rows of ramp, and the mark goes in the space that
# frees up, shaded like the rows it replaces.
#
# Each mark is (rows needed, above/below, pixels as (row, column)).
MENU_MARKS = {
    "dia":   (2, "above", [(0, 1), (0, 2), (0, 5), (0, 6),
                           (1, 1), (1, 2), (1, 5), (1, 6)]),
    "acute": (2, "above", [(0, 4), (0, 5), (1, 3), (1, 4)]),
    "grave": (2, "above", [(0, 2), (0, 3), (1, 3), (1, 4)]),
    "circ":  (2, "above", [(0, 3), (0, 4), (1, 2), (1, 5)]),
    "tilde": (2, "above", [(0, 2), (0, 3), (0, 6),
                           (1, 1), (1, 4), (1, 5)]),
    "ring":  (3, "above", [(0, 3), (0, 4), (1, 2), (1, 5), (2, 3), (2, 4)]),
    "ced":   (2, "below", [(0, 3), (0, 4), (1, 2), (1, 3)]),
}
MENU_ACCENTED = {}
for _base, _mark, _set in (
        ("AEIOUY", "dia",   "ÄËÏÖÜŸ"),
        ("AEIOUY", "acute", "ÁÉÍÓÚÝ"),
        ("AEIOU",  "grave", "ÀÈÌÒÙ"),
        ("AEIOU",  "circ",  "ÂÊÎÔÛ"),
        ("ANO",    "tilde", "ÃÑÕ"),
        ("A",      "ring",  "Å"),
        ("C",      "ced",   "Ç")):
    for _b, _c in zip(_base, _set):
        MENU_ACCENTED[_c] = (_b, _mark)


def _tile_pixels(b):
    """32 bytes of 4bpp -> 8 rows of 8 palette indices"""
    rows = []
    for y in range(8):
        r = []
        for x in range(8):
            m = 0x80 >> x
            r.append((1 if b[y * 2] & m else 0)
                     | (2 if b[y * 2 + 1] & m else 0)
                     | (4 if b[16 + y * 2] & m else 0)
                     | (8 if b[16 + y * 2 + 1] & m else 0))
        rows.append(r)
    return rows


def _tile_bytes(rows):
    """8 rows of 8 palette indices -> 32 bytes of 4bpp"""
    b = bytearray(32)
    for y in range(8):
        for x in range(8):
            p, m = rows[y][x], 0x80 >> x
            if p & 1: b[y * 2] |= m
            if p & 2: b[y * 2 + 1] |= m
            if p & 4: b[16 + y * 2] |= m
            if p & 8: b[16 + y * 2 + 1] |= m
    return bytes(b)


def _squash(rows, n):
    """drop n rows from the middle, the least distinct ones first"""
    rows, hi = [r[:] for r in rows], 14
    for _ in range(n):
        cost = [(sum(1 for a, b in zip(rows[r], rows[r - 1]) if a != b), r)
                for r in range(2, hi)]
        rows.pop(min(cost)[1])
        hi -= 1
    return rows


def menu_glyph(art, ch):
    """(top 32 bytes, bottom 32 bytes) for one character, or None"""
    ch = ch.upper()
    if ch == " ":
        return bytes(32), bytes(32)
    t = MENU_FONT.get(ch)
    if t is not None:
        return art[t * 32:(t + 1) * 32], art[(t + 16) * 32:(t + 17) * 32]
    acc = MENU_ACCENTED.get(ch)
    if acc is None:
        return None
    base, mark = acc
    n, side, pixels = MENU_MARKS[mark]
    t = MENU_FONT[base]
    cell = (_tile_pixels(art[t * 32:(t + 1) * 32])
            + _tile_pixels(art[(t + 16) * 32:(t + 17) * 32]))
    at = list(range(n)) if side == "above" else list(range(16 - n, 16))
    shade = []
    for r in at:
        seen = set(cell[r]) - {0}
        shade.append(max(seen, key=cell[r].count) if seen else 1)
    body, blank = _squash(cell, n), [[0] * 8 for _ in range(n)]
    out = blank + body if side == "above" else body + blank
    for r, c in pixels:
        out[at[r]][c] = shade[r]
    return _tile_bytes(out[:8]), _tile_bytes(out[8:])


def menu_slot(k):
    """artwork tile for the k'th sprite in the pool

    A 16x16 sprite reads tiles T, T+1, T+16 and T+17, so it owns two adjacent
    columns of a band and can never straddle one: a band is sixteen columns,
    which is exactly eight sprites. Allocating per sprite rather than per line
    means a line may be split across bands, which costs nothing -- every entry
    carries its own tile word -- and wastes no columns.
    """
    band, col = divmod(k, MENU_BAND_COLS // 2)
    if band >= len(MENU_FREE_BANDS):
        sys.exit("the menu needs %d free artwork bands and %d are listed"
                 % (band + 1, len(MENU_FREE_BANDS)))
    return MENU_FREE_BANDS[band] + col * 2


def menu_flags(rom, xbyte):
    """{sprite address: x byte} -> spans rewriting the chunks' flags words

    The flags word is not decoration. Decompiled at $00:8EDF the emitter takes
    TWO bits from it per sprite: the first is bit 8 of X, OR'd into the entry's
    byte BEFORE the base is added, and the second is the 16x16 size bit. So
    for an on-screen sprite the first bit has to be whatever carry the 8-bit
    sum produces -- it is really a sign extension. An entry byte of $C2 with
    base 136 adds to $14A, and only the flag bit, making it $24A, keeps bit 8
    of the result clear.

    Writing new X bytes and leaving the US flags alone is what put five
    sprites 256 pixels right of where they belonged. Bits outside the sprites
    actually written are preserved, which is what keeps the record's
    terminator -- an x byte of $00 whose flag bit is set, tested at $00:8EE7.
    """
    spans = []
    for addr, count in MENU_CHUNKS:
        off = addr - 0x8000
        flags = rom[off] | (rom[off + 1] << 8)
        for i in range(count):
            x = xbyte.get(addr + 2 + i * 4)
            if x is None:
                continue
            carry = 1 if x + MENU_BASE_X >= 0x100 else 0
            flags &= ~(3 << (i * 2))
            flags |= (carry | 2) << (i * 2)
        spans.append((off, bytes([flags & 0xff, (flags >> 8) & 0xff])))
    return spans


def menu_lines_spans(us, art, lines):
    """{screen y: text} -> (artwork spans, cart spans)

    Lays the three option lines out from scratch. Each sprite is 16x16 and
    carries two characters, so a line of n characters costs ceil(n / 2) of the
    eighteen in the pool, and the pool is shared: a long line borrows from a
    short one. Every entry's x, y and tile word is written, and so is each
    chunk's flags word, so nothing is inherited from the US layout but the
    palette and priority bits. Spare sprites are pointed at a blank pair
    rather than left drawing the fragment of SCENARIO they used to.
    """
    order = [y for y in MENU_LINE_Y if lines.get(y)]
    texts = [lines[y].upper() for y in order]
    need = [(len(t) + 1) // 2 for t in texts]
    if sum(need) > len(MENU_SPRITES):
        sys.exit("these lines need %d sprites and the menu has %d, which is "
                 "%d characters in all: %s"
                 % (sum(need), len(MENU_SPRITES), len(MENU_SPRITES) * 2,
                    ", ".join('"%s" = %d' % (t, n)
                              for t, n in zip(texts, need))))
    art_spans, cart, xbyte, slot = [], [], {}, 0

    def place(k, x, y, tile):
        addr = MENU_SPRITES[k]
        off = addr - 0x8000
        xb = (x - MENU_BASE_X) & 0xff
        xbyte[addr] = xb
        attr = us[off + 3] & 0xfe
        cart.append((off, bytes([xb, (y - MENU_BASE_Y) & 0xff, tile & 0xff,
                                 ((tile >> 8) & 1) | attr])))

    for y, text, n in zip(order, texts, need):
        padded = text.ljust(n * 2)
        missing = sorted(set(c for c in padded if menu_glyph(art, c) is None))
        if missing:
            sys.exit("no glyph for %s -- the font is A-Z, ! ? - . and the "
                     "accented letters listed in MENU_ACCENTED"
                     % " ".join("%s (U+%04X)" % (c, ord(c))
                                for c in missing))
        first = menu_slot(slot)
        for i in range(n):
            t = menu_slot(slot)
            for half in (0, 1):
                top, bot = menu_glyph(art, padded[i * 2 + half])
                art_spans += [(t + half, top), (t + 16 + half, bot)]
            place(slot, MENU_LINE_X0 + i * 16, y, t)
            slot += 1
        print('  y=%-3d  %-18s %2d sprites, artwork tile $%03X'
              % (y, '"' + text + '"', n, first))
    for k in range(slot, len(MENU_SPRITES)):
        t = menu_slot(k)
        for d in (0, 1, 16, 17):
            art_spans.append((t + d, bytes(32)))
        place(k, MENU_LINE_X0, MENU_LINE_Y[0], t)
    spare = len(MENU_SPRITES) - slot
    if spare:
        print("  %d spare sprite%s pointed at a blank pair"
              % (spare, "" if spare == 1 else "s"))
    cart += menu_flags(us, xbyte)
    bands = -(-len(MENU_SPRITES) // (MENU_BAND_COLS // 2))
    print("  artwork rows %s, %d chunk flags words rewritten"
          % (", ".join("%d-%d" % (MENU_FREE_BANDS[b] // 16,
                                  MENU_FREE_BANDS[b] // 16 + 1)
                       for b in range(bands)), len(MENU_CHUNKS)))
    return art_spans, cart

def _menu_record(rom, ptr):
    """(bytes, sprite count) for one record, respecting its real terminator"""
    a = ptr - 0x8000
    flags = rom[a] | (rom[a + 1] << 8)
    n = 0
    for i in range(8):
        if rom[a + 2 + i * 4] == 0 and ((flags >> (i * 2)) & 1):
            break
        n += 1
    ln = 2 + n * 4 + (1 if n < 8 else 0)
    return rom[a:a + ln], n


def menu_spans(us, dn):
    """donor menu records, relocated into filler, plus the repointed table"""
    spans, at = [], MENU_FREE
    for idx in MENU_INDICES:
        t = MENU_TABLE + idx * 2
        dptr = dn[t] | (dn[t + 1] << 8)
        rec, n = _menu_record(dn, dptr)
        if at + len(rec) > MENU_FREE_END:
            sys.exit("menu records do not fit in the filler at $%06X" % MENU_FREE)
        spans.append((at, bytes(rec)))
        new = 0x8000 + (at - 0x000000)       # bank $00: file offset -> address
        spans.append((t, bytes([new & 0xff, (new >> 8) & 0xff])))
        print("  idx $%02X: %d sprites, %d bytes, donor $%04X -> $%04X"
              % (idx, n, len(rec), dptr, new))
        at += len(rec)
    print("  %d bytes used of the %d free at $%06X"
          % (at - MENU_FREE, MENU_FREE_END - MENU_FREE, MENU_FREE))
    return spans


def _lz5():
    """the decompressor from extract_graphics.py, loaded as a module"""
    import importlib.util
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location(
        "extract_graphics", os.path.join(here, "extract_graphics.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def scan_packets(rom, eg, minlen=1024):
    """every LZ5 stream in `rom` that unpacks to at least `minlen` bytes"""
    out, off, n = [], 0, len(rom)
    while off < n:
        try:
            d, end = eg.nintendo_decompress(rom, off)
        except Exception:
            off += 1
            continue
        if len(d) >= minlen and (end - off) >= 64 and len(d) > (end - off):
            out.append((off, bytes(d)))
            off = end
        else:
            off += 1
    return out


def find_twin(packets, want):
    """the donor packet that is this US packet's counterpart

    Same decompressed length, then best byte agreement. Length alone is not
    enough -- the twelve briefing pages are all 4096 bytes -- and agreement
    alone is not enough either, so both are required.
    """
    best = None
    for off, d in packets:
        if len(d) != len(want):
            continue
        same = sum(1 for i in range(len(d)) if d[i] == want[i])
        if best is None or same > best[1]:
            best = (off, same, d)
    return best


def cmd_packets(a):
    eg = _lz5()
    us = open(a.rom, "rb").read()
    dn = open(a.donor, "rb").read()
    umap, _ = eg.nintendo_decompress(us, SELECTOR_MAP)
    uchr, _ = eg.nintendo_decompress(us, SELECTOR_CHR)
    pk = scan_packets(dn, eg, 4096)
    tmap = find_twin(pk, umap)
    tchr = find_twin(pk, uchr)
    if not tmap or not tchr:
        sys.exit("no counterpart for the selector packets in %s -- is this a "
                 "SimCity ROM of another region?" % os.path.basename(a.donor))
    dmapoff, magree, dmap = tmap
    dchroff, cagree, dchr = tchr
    print("selector tilemap  us $%06X  <-  donor $%06X  (%d/%d bytes agree)"
          % (SELECTOR_MAP, dmapoff, magree, len(umap)))
    print("selector artwork  us $%06X  <-  donor $%06X  (%d/%d bytes agree)"
          % (SELECTOR_CHR, dchroff, cagree, len(uchr)))

    uw = [umap[i] | (umap[i + 1] << 8) for i in range(0, len(umap), 2)]
    dw = [dmap[i] | (dmap[i + 1] << 8) for i in range(0, len(dmap), 2)]
    art = lambda c, t: bytes(c[t * CHR_BYTES_PER_TILE:
                              (t + 1) * CHR_BYTES_PER_TILE])

    # A cell can only be taken if its artwork can be: tiles at or past 511 are
    # served by a CHR block this patch does not carry, so those cells keep the
    # US entry rather than pointing at whatever happens to sit there. Four
    # cells on one card do this; the rest of the screen is unaffected.
    out_map = bytearray(umap)
    kept, taken, skipped = 0, 0, 0
    need = set()
    for i, (u, d) in enumerate(zip(uw, dw)):
        if u == d:
            kept += 1
            continue
        if (d & 0x3ff) >= CHR_TILES:
            skipped += 1
            continue
        out_map[i * 2] = d & 0xff
        out_map[i * 2 + 1] = (d >> 8) & 0xff
        need.add(d & 0x3ff)
        taken += 1

    # Overwriting a tile is only safe if nothing left on the screen still
    # wants the US art at that index. Checked, not assumed.
    still = set(uw[i] & 0x3ff for i in range(len(uw))
                if out_map[i * 2] | (out_map[i * 2 + 1] << 8) == uw[i])
    spans, clashes = [], []
    for t in sorted(need):
        if art(uchr, t) == art(dchr, t):
            continue
        if t in still:
            clashes.append(t)
            continue
        spans.append((t * CHR_BYTES_PER_TILE, art(dchr, t)))
    if clashes:
        sys.exit("tiles %s carry different art in the donor but are still used "
                 "elsewhere on the US selector -- this patch would corrupt it"
                 % clashes)
    print("  %d cells translated, %d unchanged, %d left as US (art not in this "
          "packet)" % (taken, kept, skipped))
    print("  %d tiles of artwork copied from the donor" % len(spans))

    # Sylt's card is this project's own artwork and carries its disaster line
    # as DRAWN pixels ("Coastal" / "Flooding"), not as strip references -- so
    # the packet patch above cannot reach it. Give it the donor's word by
    # copying the strip the shipped layout uses for the same disaster: Rio's,
    # at row 11 columns 23-28 of the tilemap. In the German ROM that reads
    # "Hochwasser". This rides along as a pseudo-entry the host applies to the
    # card's tile data instead of to a ROM packet.
    sylt = []
    for k in range(SYLT_STRIP_LEN):
        t = dw[SYLT_SRC_ROW * 64 + SYLT_SRC_COL + k] & 0x3ff
        slot = SYLT_CARD_ROW * SYLT_CARD_W + SYLT_CARD_COL + k
        sylt.append((slot * CHR_BYTES_PER_TILE, art(dchr, t)))
    print("  Sylt's disaster line: %d tiles from the donor's own strip"
          % len(sylt))

    # --swap takes a whole packet from the donor rather than reasoning about
    # its cells. The menus need this: their tilemaps are byte-identical
    # across regions while the artwork differs, so the words are placed by
    # code from fixed slots and only the pictures change. Whether the German
    # strips really occupy the same slots is a question for the screen, not
    # for analysis -- this is how it gets asked.
    # --rom-copy LO-HI lays the donor's bytes over that range of the cart
    # image. Only the 32-byte tiles that actually differ are carried, so a
    # generous range costs nothing and cannot drag in unrelated art.
    rom_spans = []
    for spec in (a.rom_copy or []):
        lo, _, hi = spec.partition("-")
        lo, hi = int(lo, 0), int(hi, 0)
        n = 0
        for t in range(lo, hi, 32):
            if us[t:t + 32] != dn[t:t + 32]:
                rom_spans.append((t, dn[t:t + 32])); n += 1
        print("rom copy $%06X-$%06X: %d of %d tiles differ, %d bytes"
              % (lo, hi, n, (hi - lo) // 32, n * 32))
    if a.menu:
        print("main menu: relocating the donor's four records")
        rom_spans += menu_spans(us, dn)
        if not a.swap or hex(MENU_ART) not in [hex(int(x, 0)) for x in a.swap]:
            print("  NOTE: --menu needs --swap 0x%06X too; the records index"
                  " the donor's artwork" % MENU_ART)
    if a.labels_from:
        sp = label_spans(a.labels_from, us)
        rom_spans += sp
        print("building labels from %s: %d tiles changed"
              % (os.path.basename(a.labels_from), len(sp)))
    if a.hud:
        lo, hi = HUD_LABELS
        n = 0
        for t in range(lo, hi, 32):
            if us[t:t + 32] != dn[t:t + 32]:
                rom_spans.append((t, dn[t:t + 32])); n += 1
        print("building labels $%06X-$%06X: %d tiles taken from the donor"
              % (lo, hi, n))
        rlo, rhi = HUD_RECORDS
        rn = 0
        for t in range(rlo, rhi, 4):
            if us[t:t + 4] != dn[t:t + 4]:
                rom_spans.append((t, dn[t:t + 4])); rn += 1
        print("  placement records $%06X-$%06X: %d sprites repositioned"
              % (rlo, rhi, rn))

    extra = []
    for spec in (a.swap or []):
        off = int(spec, 0)
        d, _ = eg.nintendo_decompress(us, off)
        tw = find_twin(scan_packets(dn, eg, len(d)), d)
        if not tw:
            sys.exit("no donor counterpart for $%06X (length %d)" % (off, len(d)))
        toff, agree, td = tw
        extra.append((off, len(d), [(0, bytes(td))]))
        print("swap $%06X  <-  donor $%06X  (%d/%d bytes agree, %d differ)"
              % (off, toff, agree, len(d), len(d) - agree))

    blob = bytearray(PACKET_MAGIC + bytes([1, 0]))
    if a.menu_text:
        field = [t.strip() for t in a.menu_text.split("|")]
        if len(field) > len(MENU_LINE_Y):
            sys.exit("--menu-text takes at most %d lines separated by |"
                     % len(MENU_LINE_Y))
        art, _ = eg.nintendo_decompress(us, MENU_ART)
        lines = dict(zip(MENU_LINE_Y, field))
        for y, t in zip(MENU_LINE_Y, field):
            if not t:
                sys.exit("line y=%d was left empty. Every line has to be "
                         "given: the eighteen sprites are one pool, so "
                         "changing any line re-lays all three" % y)
        print("main menu text:")
        art_spans, cart = menu_lines_spans(us, art, lines)
        extra.append((MENU_ART, len(art),
                      [(t * 32, d) for t, d in art_spans]))
        rom_spans += cart

    entries = [(SELECTOR_MAP, len(umap), [(0, bytes(out_map))]),
               (SELECTOR_CHR, len(uchr), spans),
               (SYLT_CARD_PSEUDO, 0, sylt)] + extra
    if rom_spans:
        entries.append((ROM_SPAN_PSEUDO, 0, rom_spans))
    blob += len(entries).to_bytes(2, "little")
    for src, outlen, sp in entries:
        if src == SYLT_CARD_PSEUDO:
            bank, addr = 0xff, 0xffff
        elif src == ROM_SPAN_PSEUDO:
            bank, addr = 0xfe, 0xffff
        else:
            bank = src // 0x8000
            addr = 0x8000 + (src % 0x8000)
        blob += bytes([bank]) + addr.to_bytes(2, "little")
        blob += outlen.to_bytes(4, "little")
        blob += len(sp).to_bytes(2, "little")
        for off, data in sp:
            blob += off.to_bytes(4, "little") + len(data).to_bytes(2, "little")
            blob += data
    open(a.out, "wb").write(blob)
    print("  %d bytes -> %s" % (len(blob), a.out))



# -- the main map's building labels ---------------------------------------
# These are the toolbar's two-line names. Unlike everything else translated
# here they are NOT in a packet: they sit uncompressed at file $034C00 and are
# copied to VRAM byte $CC00 one tile for one. Found by taking tiles straight
# out of a live capture and searching the ROM -- 70 of 80 matched consecutively
# from that base, and none appeared in any compressed packet.
#
# Taking the donor's block wholesale does NOT work. The words are pre-rendered
# strips, the code slices fixed tile ranges out of them, and the German words
# sit at different offsets, so "Bahngleis" came out in play as "n-en lei /
# tung Parkal". The artwork copied; the slicing did not.
#
# So the block is exported as an ordinary image instead. A translator paints
# both lines of each label where they belong -- the slices stay exactly where
# the game expects them -- and imports it back. No donor ROM needed, and no
# 65816 either.
# The block runs further than it looks. Sprite tile $060 is its first tile,
# so tile T sits at LABEL_BASE + (T - 0x60) * 32, and the records reach tile
# $1B5 -- ROM $0376C0. Stopping at $036200 (tile $110) left twelve tiles
# behind, among them the second lines of Wohn-/Gewerbe-/Industrie-, which in
# play showed as "Wohn- / Nucle": German first line, English second.
LABEL_BASE, LABEL_END = 0x034C00, 0x0376C0
LABEL_COLS = 16                  # the block is 16 tiles wide
LABEL_BPP = 4

# One visually distinct colour per 4bpp index, so an editor can pick them
# apart. Import maps back by nearest, exactly like tools/make_sylt_card.py.
LABEL_PAL = [(0, 0, 0), (255, 255, 255), (170, 170, 170), (85, 85, 85),
             (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0),
             (255, 0, 255), (0, 255, 255), (128, 0, 0), (0, 128, 0),
             (0, 0, 128), (128, 128, 0), (128, 0, 128), (0, 128, 128)]


def _label_tiles(blob):
    """(index per pixel) for each 4bpp tile in the block"""
    out = []
    for t in range(len(blob) // 32):
        px = [[0] * 8 for _ in range(8)]
        for y in range(8):
            p0, p1 = blob[t * 32 + y * 2], blob[t * 32 + y * 2 + 1]
            p2, p3 = blob[t * 32 + 16 + y * 2], blob[t * 32 + 16 + y * 2 + 1]
            for x in range(8):
                b = 7 - x
                px[y][x] = (((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) |
                            (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3))
        out.append(px)
    return out


def _tile_bytes(px):
    """the inverse: 8x8 of 4bpp indices -> 32 bytes of SNES planar"""
    out = bytearray(32)
    for y in range(8):
        p0 = p1 = p2 = p3 = 0
        for x in range(8):
            v = px[y][x] & 0xf
            b = 7 - x
            p0 |= (v & 1) << b
            p1 |= ((v >> 1) & 1) << b
            p2 |= ((v >> 2) & 1) << b
            p3 |= ((v >> 3) & 1) << b
        out[y * 2], out[y * 2 + 1] = p0, p1
        out[16 + y * 2], out[16 + y * 2 + 1] = p2, p3
    return bytes(out)


def cmd_labels(a):
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    rom = open(a.rom, "rb").read()
    blob = rom[LABEL_BASE:LABEL_END]
    tiles = _label_tiles(blob)
    rows = (len(tiles) + LABEL_COLS - 1) // LABEL_COLS
    img = PIL.Image.new("P", (LABEL_COLS * 8, rows * 8), 0)
    pal = []
    for c in LABEL_PAL:
        pal += list(c)
    img.putpalette(pal + [0] * (768 - len(pal)))
    px = img.load()
    for i, t in enumerate(tiles):
        ox, oy = (i % LABEL_COLS) * 8, (i // LABEL_COLS) * 8
        for y in range(8):
            for x in range(8):
                px[ox + x, oy + y] = t[y][x]
    img.save(a.out)
    print("building labels $%06X-$%06X: %d tiles, %dx%d -> %s"
          % (LABEL_BASE, LABEL_END, len(tiles), img.width, img.height, a.out))
    print("  16 tiles a row; each label is TWO rows, and the game slices fixed")
    print("  tile ranges out of them -- keep every word inside the columns it")
    print("  already occupies, and use both lines rather than overrunning one.")


def label_spans(path, rom):
    """an edited label image -> (rom offset, 32 bytes) spans for what changed"""
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    im = PIL.Image.open(path).convert("RGB")
    if im.width != LABEL_COLS * 8:
        sys.exit("%s is %d px wide; the label block is %d"
                 % (path, im.width, LABEL_COLS * 8))
    src = im.load()

    def nearest(c):
        best, bi = None, 0
        for i, p in enumerate(LABEL_PAL):
            d = sum((c[k] - p[k]) ** 2 for k in range(3))
            if best is None or d < best:
                best, bi = d, i
        return bi

    spans, n = [], (im.width // 8) * (im.height // 8)
    for i in range(n):
        off = LABEL_BASE + i * 32
        if off + 32 > LABEL_END:
            break
        ox, oy = (i % LABEL_COLS) * 8, (i // LABEL_COLS) * 8
        px = [[nearest(src[ox + x, oy + y]) for x in range(8)] for y in range(8)]
        b = _tile_bytes(px)
        if b != rom[off:off + 32]:
            spans.append((off, b))
    return spans

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
    i.add_argument("--briefs", action="store_true",
                   help="also take the donor's briefings, as strings")
    i.add_argument("--no-glyphs", action="store_true",
                   help="skip the accent glyphs (umlauts and accents)")
    i.add_argument("--surface", action="append", metavar="FILE",
                   help="a surface file to apply (repeatable)")
    i.add_argument("--cards-from", metavar="DIR",
                   help="scenario cards exported from a donor selector capture")
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
    bp = sub.add_parser("briefs-pack", help="edited briefing strings -> blob")
    bp.set_defaults(fn=cmd_briefs_pack)
    bp.add_argument("--in", required=True); bp.add_argument("--out", required=True)
    bp.add_argument("--text-from", metavar="JSON",
                    help="take the words from another region's export, "
                         "keeping this one's addresses")
    b = sub.add_parser("briefs", help="export briefing pages as strings")
    b.set_defaults(fn=cmd_briefs)
    b.add_argument("--rom", required=True); b.add_argument("--out", required=True)
    cd = sub.add_parser("cards", help="export the scenario cards from a VRAM dump")
    cd.set_defaults(fn=cmd_cards)
    cd.add_argument("--vram", required=True, help="SC_VRAM_DUMP taken on screen $0b")
    cd.add_argument("--out", required=True)
    cd.add_argument("--map", type=lambda x: int(x, 0), default=0x3000)
    cd.add_argument("--chr", type=lambda x: int(x, 0), default=0x0000)
    sf = sub.add_parser("surface", help="capture a translated screen region")
    sf.set_defaults(fn=cmd_surface)
    sf.add_argument("--donor", required=True, help="VRAM dump from the donor ROM")
    sf.add_argument("--target", required=True, help="VRAM dump from the US build")
    sf.add_argument("--layer", type=int, default=3, choices=[1, 2, 3, 4])
    sf.add_argument("--rows", required=True, help="e.g. 12-15,23-26")
    sf.add_argument("--cols", help="e.g. 2-9,12-19 -- REQUIRED in practice: a "
                                   "full-width row overwrites the background")
    sf.add_argument("--out", required=True)
    pc = sub.add_parser("packets", help="patch the selector at its ROM source")
    pc.set_defaults(fn=cmd_packets)
    pc.add_argument("--rom", default="Sim City (U) [!].sfc",
                    help="the US image the patch targets")
    pc.add_argument("--donor", required=True, help="a translated ROM")
    pc.add_argument("--out", required=True)
    pc.add_argument("--labels-from", metavar="PNG",
                    help="an edited building-label image (text_tool.py labels)")
    pc.add_argument("--menu-text", metavar="TOP|MIDDLE|BOTTOM",
                    help="the main menu's three option lines, e.g. "
                         "\"UBUNGSSPIEL|NEUE STADT|SCHAUPLATZE\". Leave a "
                         "field empty to keep the US wording. 36 characters "
                         "in all, and any three splits of that; accented "
                         "letters are built on the fly")
    pc.add_argument("--menu", action="store_true",
                    help="RETRACTED: relocates donor records and loses the "
                         "logo. Use --menu-text")
    pc.add_argument("--hud", action="store_true",
                    help="take the main map's building labels from the donor")
    pc.add_argument("--rom-copy", action="append", metavar="LO-HI",
                    help="lay the donor's bytes over this cart-image range; "
                         "repeatable")
    pc.add_argument("--swap", action="append", metavar="ADDR",
                    help="take this whole US packet from the donor "
                         "(a file offset, e.g. 0x04A571); repeatable")
    lb = sub.add_parser("labels", help="export the main map's building "
                                       "labels as an editable image")
    lb.set_defaults(fn=cmd_labels)
    lb.add_argument("--rom", default="Sim City (U) [!].sfc")
    lb.add_argument("--out", required=True)
    g = sub.add_parser("glyphs"); g.set_defaults(fn=cmd_glyphs)
    g.add_argument("--rom", required=True); g.add_argument("--version", required=True)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
