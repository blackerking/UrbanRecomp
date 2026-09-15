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


# Where the accent glyphs go in the US message font.
#
# They used to go at their CP437 codes, $81..$9B, on the grounds that the US
# message records never use those. That is true and not enough: the in-city
# notice table in bank 01 ($01:9824..$01:9C9C -- "More Residential zones
# needed", "Save completed.") draws from the same font with every character
# stored as its code plus $60, so that table's space, punctuation and digits
# ARE $80..$9F. An accent written over $8E turned its full stop into an
# accented letter, reported from play as "Save completed" ending in a wrong
# character, and the same collision hit "!", ",", "3", "4" and "7".
#
# Tiles $E0..$FE are blank in the US font and used by neither the messages
# nor that table, so the accents go there, in order, and the translated
# records are rewritten to point at them.
ACCENT_SLOT_BASE = 0xE0


def _accent_pairs(donor_rom, donor):
    """(code, 16 raw bytes) for each accent the donor's font actually draws"""
    dn, dn_off = font_raw(donor_rom, donor)
    out = []
    for code in ACCENT_CODES:
        di = code - dn_off
        if (di + 1) * FONT_RAW_TILE > len(dn):
            continue
        glyph = dn[di * FONT_RAW_TILE:(di + 1) * FONT_RAW_TILE]
        if glyph != bytes(FONT_RAW_TILE):
            out.append((code, glyph))
    return out


def accent_glyphs(donor_rom, donor, us_rom):
    """(font slot, 16 raw bytes) for each accent, in the free slots from $E0"""
    return [(ACCENT_SLOT_BASE + k, g)
            for k, (_, g) in enumerate(_accent_pairs(donor_rom, donor))]


def remap_accents(recs, donor_rom, donor):
    """message records with every accent code pointed at its new font slot"""
    table = dict((code, ACCENT_SLOT_BASE + k)
                 for k, (code, _) in enumerate(_accent_pairs(donor_rom, donor)))
    return [bytes(table.get(b, b) for b in r) for r in recs]


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
    if glyphs:
        recs = remap_accents(recs, a.donor, donor)

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
# The screen the menu unpacks it on. It is not the only one: the scenario
# selector unpacks the same packet on $0a and draws its win marks from it
# (record $29, tiles $1B0 $1B2 $1D0 $1D2), and the new-city screens unpack it
# on $04. Glyphs composed for the menu are therefore applied only on the
# screens the menu's own loader ($02:BB23) runs on: $02, reached from the
# title, back from the selector and back from the new-city screens, and $12,
# the way back from a city. Scoping to $02 alone left the menu blank after
# returning from a city -- reported from play, and traced to the loader
# unpacking on $12.
MENU_SCREENS = (0x02, 0x12)

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
# Because x, y and tile all live in the entry, they are a free pool: any of
# them can be given to any line. That is what lifts the eight-character cap.
# The US layout spends 4 + 7 + 7 of its eighteen, but nothing fixes that split.
#
#   $A3CE  8 sprites   y=160 x131..179  then  y=112 x74..122
#   $A3F0  8 sprites   y=136 x74..170   then  y=160 x106
#   $A412  2 sprites   y=160 x74, x90
# The pool as the emitter sees it: record $0F is ONE record of chained chunks.
# $00:8F4A, reached when the eight-sprite budget runs out, jumps back to
# $00:8EBA, which reloads the budget and reads a FRESH flags word from the
# next two bytes -- so a record simply carries on, 8 sprites at a time, until
# a chunk terminates. That is why nothing points at $A3F0 or $A412.
#
# As shipped the chain is 8 + 8 + 2 and ends at $A41C, because record $10
# begins at $A41D. Growing it in place is therefore impossible -- but $10 is
# reached ONLY through the table at $00:A164, and a record's entries are
# self-contained (x, y, tile, attribute; no internal pointers), so its whole
# 61-byte chain copies verbatim into the bank-0 filler and the table entry is
# repointed at the copy. That frees $A41D onward and the chain becomes
# 8 + 8 + 8 with a fourth chunk holding nothing but the terminator.
#
# 24 sprites, 48 characters. The US wording needs 18 and the French
# cartridge's own needs 20.
MENU_CHUNKS = [(0xA3CE, 8), (0xA3F0, 8), (0xA412, 8)]
MENU_TAIL = 0xA434                       # flags word + terminator byte
MENU_MOVE = (0x10, 0xA41D, 61)           # record, where it is, chain length
# The saved-game line, record $0E: drawn only while a save exists, from base
# (136, 128), eight sprites at y=100 reading RESUME SAVED CITY. It ends where
# $0F begins ($A3A9..$A3CD), so a longer line cannot grow in place either; it
# is rebuilt in the filler after the copy of $10. The German cartridge's own
# wording is GESPEICHERTE STADT, nine sprites laid out by word.
MENU_SAVED = (0x0E, 0xA3A9, 37)          # record, where it is, chain length
MENU_SAVED_BASE = (136, 128)
MENU_SAVED_Y, MENU_SAVED_X0 = 100, 73
MENU_SPRITES = [c + 2 + i * 4 for c, n in MENU_CHUNKS for i in range(n)]
MENU_BASE_X, MENU_BASE_Y = 136, 116      # $025d / $025f for these three lines
MENU_LINE_Y = (112, 136, 160)            # the arrow's stops, from table $d37c
MENU_LINE_X0 = 74                        # all three lines are left-aligned here
MENU_LINE_MAX = 252                      # and none may run past this
MENU_PARK_Y = 224                        # spare sprites, clear of the screen

# Composed glyphs go in artwork rows 20-31, none of which the title or the
# menu displays -- established by snapshotting OAM on those screens and taking
# the union of the tiles actually used, NOT by scanning the record table.
# They are not free everywhere this packet is unpacked: the scenario selector
# draws its win marks from $1B0 $1B2 $1D0 $1D2, which is why the glyphs are
# applied on the menu's screen only (MENU_SCREEN). That scan called rows 2-3 free; composing there broke START
# NEW CITY, which draws tile $022 from a chunk the scan never reached.
# A band is a pair of rows: a character's top half sits in the first and its
# bottom half sixteen tiles later, in the second. Rows 30-31 are blank as well
# as unused, so they come first.
# Rows 24-27 are NOT free, whatever the sweep said: the save list (screen $11,
# reached through RESUME SAVED CITY) draws its digits from this artwork as 8x8
# sprites, tops at $190-$199 and bottoms at $1A0-$1A9. The sweep never visited
# that screen, and the ninth sprite of GESPEICHERTE STADT spilled into $180,
# whose lower half is $190/$191 -- reported from play as the "1" of "1." and
# "196." missing its top.
MENU_FREE_BANDS = (0x1E0, 0x1C0, 0x140, 0x160)
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


def menu_layout(text):
    """text -> [(x offset, the sprite's two characters)]

    Words are packed whole, which is how both cartridges do it. A word of n
    letters takes ceil(n / 2) sprites, and an odd-length word leaves its last
    half blank -- that blank IS the space before the next word. Only after an
    even-length word does the space cost anything, and then it costs 8 pixels
    of position rather than a sprite.

    Checked against the French cartridge, which is the one that spends its
    sprites carefully: NOUVELLE CITE comes out at offsets 0 16 32 48 72 88,
    exactly its own, and CHOISIS SCENARIO within a pixel of its own.
    """
    out, x = [], 0
    for i, word in enumerate(w for w in text.split(" ") if w):
        if i:
            x += 8
        pad = word if len(word) % 2 == 0 else word + " "
        for k in range(0, len(pad), 2):
            out.append((x, pad[k:k + 2]))
            x += 16
        if len(word) % 2:
            x -= 8
    return out


def menu_relocate(rom):
    """spans that move record $10 aside and close the grown chain

    Record $0F ends where record $10 begins, so the chain can only grow if $10
    moves. It is copied verbatim -- entries carry no internal pointers -- into
    the bank-0 filler, the table entry is repointed, and a fourth chunk holding
    only a terminator is written past the new eight-sprite third chunk.
    """
    idx, addr, length = MENU_MOVE
    src = addr - 0x8000
    ptr = 0x8000 + (MENU_FREE % 0x8000)
    if MENU_FREE + length > MENU_FREE_END:
        sys.exit("record $%02X does not fit in the filler at $%06X"
                 % (idx, MENU_FREE))
    return [(MENU_FREE, rom[src:src + length]),
            (MENU_TABLE + idx * 2, bytes([ptr & 0xff, ptr >> 8])),
            (MENU_TAIL - 0x8000, bytes([0x01, 0x00, 0x00]))]


def menu_saved_spans(us, art, text):
    """the saved-game line, record $0E, laid out afresh -> (art spans, cart spans)

    Its glyphs take the pool slots after the option lines' 24, in the same
    screen-scoped artwork entry, and the record is rebuilt whole: flags words
    with the X-carry and size bits, the entries, and a one-byte terminator.
    """
    idx, addr, length = MENU_SAVED
    bx, by = MENU_SAVED_BASE
    text = text.upper()
    cells = menu_layout(text)
    if not cells:
        sys.exit("the saved-game line is empty")
    end = MENU_SAVED_X0 + cells[-1][0] + 16
    if end > MENU_LINE_MAX:
        sys.exit('"%s" runs to x=%d; the saved-game line must end by x=%d'
                 % (text, end, MENU_LINE_MAX))
    missing = sorted(set(c for _, pair in cells for c in pair
                         if menu_glyph(art, c) is None))
    if missing:
        sys.exit("no glyph for %s in the saved-game line"
                 % " ".join("%s (U+%04X)" % (c, ord(c)) for c in missing))
    attr = us[addr - 0x8000 + 5] & 0xfe
    art_spans, entries = [], []
    for k, (dx, pair) in enumerate(cells):
        t = menu_slot(getattr(menu_lines_spans, "next_slot",
                              len(MENU_SPRITES)) + k)
        for half in (0, 1):
            top, bot = menu_glyph(art, pair[half])
            art_spans += [(t + half, top), (t + 16 + half, bot)]
        entries.append(((MENU_SAVED_X0 + dx - bx) & 0xff,
                        (MENU_SAVED_Y - by) & 0xff, t))
    rec = bytearray()
    for c0 in range(0, len(entries) + 1, 8):
        part = entries[c0:c0 + 8]
        flags, body = 0, bytearray()
        for i, (xb, yb, t) in enumerate(part):
            carry = 1 if xb + bx >= 0x100 else 0
            flags |= (carry | 2) << (i * 2)
            body += bytes([xb, yb, t & 0xff, ((t >> 8) & 1) | attr])
        if len(part) < 8:
            flags |= 1 << (len(part) * 2)
            body += bytes([0])
        rec += bytes([flags & 0xff, (flags >> 8) & 0xff]) + body
        if len(part) < 8:
            break
    at = MENU_FREE + MENU_MOVE[2]
    if at + len(rec) > MENU_FREE_END:
        sys.exit("the saved-game line does not fit in the filler at $%06X" % at)
    ptr = 0x8000 + (at % 0x8000)
    print('  saved line "%s": %d sprites, record $%02X rebuilt at $%06X'
          % (text, len(entries), idx, at))
    return art_spans, [(at, bytes(rec)),
                       (MENU_TABLE + idx * 2, bytes([ptr & 0xff, ptr >> 8]))]


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
    sprites 256 pixels right of where they belonged.
    """
    spans = []
    for addr, count in MENU_CHUNKS:
        flags = 0
        for i in range(count):
            x = xbyte[addr + 2 + i * 4]
            carry = 1 if x + MENU_BASE_X >= 0x100 else 0
            flags |= (carry | 2) << (i * 2)
        spans.append((addr - 0x8000, bytes([flags & 0xff, flags >> 8])))
    return spans


def menu_lines_spans(us, art, lines):
    """{screen y: text} -> (artwork spans, cart spans)

    Lays the three option lines out from scratch. Each sprite is 16x16 and
    carries two characters, and the 24 in the pool are shared between the
    lines: a long line borrows from a short one. Every entry's x, y and tile
    word is written, and so is each chunk's flags word, so nothing is
    inherited from the US layout but the palette and priority bits. Spare
    sprites are parked below the screen with a blank tile rather than left
    drawing the fragment of SCENARIO they used to.
    """
    order = [y for y in MENU_LINE_Y if lines.get(y)]
    laid = [(y, lines[y].upper(), menu_layout(lines[y].upper()))
            for y in order]
    need = sum(len(c) for _, _, c in laid)
    if need > len(MENU_SPRITES):
        sys.exit("these lines need %d sprites and the menu has %d, which is "
                 "%d characters in all: %s"
                 % (need, len(MENU_SPRITES), len(MENU_SPRITES) * 2,
                    ", ".join('"%s" = %d' % (t, len(c)) for _, t, c in laid)))
    art_spans, cart, xbyte, slot = [], [], {}, 0

    # One attribute for the whole pool, taken from the US line text. It must
    # NOT be inherited per sprite: past the original eighteen the bytes at
    # those addresses are record $10's old data, so the last two sprites of a
    # French layout would come out on the wrong palette.
    attr = us[MENU_SPRITES[0] - 0x8000 + 3] & 0xfe

    def place(k, x, y, tile):
        addr = MENU_SPRITES[k]
        off = addr - 0x8000
        xb = (x - MENU_BASE_X) & 0xff
        xbyte[addr] = xb
        cart.append((off, bytes([xb, (y - MENU_BASE_Y) & 0xff, tile & 0xff,
                                 ((tile >> 8) & 1) | attr])))

    for y, text, cells in laid:
        end = MENU_LINE_X0 + cells[-1][0] + 16 if cells else MENU_LINE_X0
        if end > MENU_LINE_MAX:
            sys.exit('"%s" is %d pixels wide and runs to x=%d; a line starts '
                     "at x=%d and must end by x=%d, so about %d characters"
                     % (text, end - MENU_LINE_X0, end, MENU_LINE_X0,
                        MENU_LINE_MAX,
                        (MENU_LINE_MAX - MENU_LINE_X0) // 8))
        missing = sorted(set(c for _, pair in cells for c in pair
                             if menu_glyph(art, c) is None))
        if missing:
            sys.exit("no glyph for %s -- the font is A-Z, ! ? - . and the "
                     "accented letters listed in MENU_ACCENTED"
                     % " ".join("%s (U+%04X)" % (c, ord(c)) for c in missing))
        first = menu_slot(slot)
        for dx, pair in cells:
            t = menu_slot(slot)
            for half in (0, 1):
                top, bot = menu_glyph(art, pair[half])
                art_spans += [(t + half, top), (t + 16 + half, bot)]
            place(slot, MENU_LINE_X0 + dx, y, t)
            slot += 1
        print('  y=%-3d  %-20s %2d sprites, artwork tile $%03X, x %d-%d'
              % (y, '"' + text + '"', len(cells), first, MENU_LINE_X0, end))
    # All spare sprites point at ONE blank pair. A pair each was a waste of
    # slots the saved-game line needs, now that rows 24-27 are off limits.
    next_slot = slot
    if slot < len(MENU_SPRITES):
        t = menu_slot(slot)
        for d in (0, 1, 16, 17):
            art_spans.append((t + d, bytes(32)))
        for k in range(slot, len(MENU_SPRITES)):
            place(k, MENU_LINE_X0, MENU_PARK_Y, t)
        next_slot = slot + 1
    spare = len(MENU_SPRITES) - slot
    if spare:
        print("  %d spare sprite%s parked at y=%d with a blank tile"
              % (spare, "" if spare == 1 else "s", MENU_PARK_Y))
    cart += menu_flags(us, xbyte) + menu_relocate(us)
    menu_lines_spans.next_slot = next_slot
    print("  record $%02X relocated to $%06X so the chain could grow to "
          "%d sprites" % (MENU_MOVE[0], MENU_FREE, len(MENU_SPRITES)))
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


_SCAN_MEMO = {}


def scan_packets(rom, eg, minlen=1024):
    """every LZ5 stream in `rom` that unpacks to at least `minlen` bytes

    Keep `minlen` small and filter afterwards. A packet shorter than the
    minimum is not skipped over, so the scan walks through it byte by byte, and
    a false stream decoded from inside it can run long enough to swallow the
    start of the real packet that follows: at 32768 the map window graphics
    were not found at all, at 512 they are. Memoised, because a donor is
    scanned by several steps of one run.
    """
    key = (len(rom), hash(bytes(rom)), minlen)
    if key in _SCAN_MEMO:
        return _SCAN_MEMO[key]
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
    _SCAN_MEMO[key] = out
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


# ── in-city notices ───────────────────────────────────────────────────────
# The two-line boxes that pop up over the city: "More Residential zones
# needed.", "Blackouts reported.", the scenario countdown, "Save completed.".
# They are not among the 53 messages. Found by their reader, decompiled at
# $01:9C9D and identical in all four cartridges:
#
#   LDA $0381 ; ASL ; TAX ; LDA $0197E3,X ; STA $79    string pointer
#   LDA $0381 ; TAX ; LDA $0194FB,X ; AND #$FF ; TAX    width class
#   LDA $01978C,X ; AND #$FF ; SEC ; SBC #2 ; STA $7F   characters a line
#   two lines of exactly that many bytes, each byte ORed with $2C00 and
#   written to the tilemap AS the tile number
#
# So a byte is a tile, and the font holds a second copy of its glyphs $60
# tiles up, recoloured: a notice byte is the CP437 code plus $60. That is also
# why German ue is $E1 and the German ss $FB in the German table.
#
# The widths are a class per notice -- 12, 15, 19 or 23 characters -- and the
# German and French cartridges change the classes as well as the text, so an
# import copies both. The class table is also read by the box frame at
# $01:9797, which is what keeps the frame and its text the same size.
#
# The text runs up to the reader itself at $01:9C9D, so a longer translation
# cannot stay in place. It goes to the $FF filler at the end of bank 01 and
# all 33 pointers are rewritten -- the reader addresses bank 01 with
# LDA $010000,X, so any address there works.
#
# Accented letters: the notice copies of the glyphs sit at code + $60, which
# is exactly where the message font's accents live now ($E0..$EF). So a
# notice's accents get their own slots, $F0..$FE, and their glyphs come from
# the donor's own notice bank, already in the notice colours.
NOTICE_PTRS = 0x0097E3          # $01:97E3; bank 01 file offsets equal addresses
NOTICE_COUNT = 33
NOTICE_CLASS = 0x0094FB         # $01:94FB
NOTICE_WIDTHS = 0x00978C        # $01:978C, box width per class, text = width - 2
NOTICE_CLASSES = 4
NOTICE_FREE = (0x00F924, 0x010000)
NOTICE_BIAS = 0x60
NOTICE_GLYPH_SLOTS = tuple(range(0xF0, 0xFF))
FONT_PACKET = 0x04C0FB          # $09:C0FB, the in-city BG3 font the notices use
FONT_TILES = 640


def notice_widths(rom):
    return [rom[NOTICE_WIDTHS + c] - 2 for c in range(NOTICE_CLASSES)]


def read_notices(rom):
    """[{id, width, lines: [first, second]}] for the 33 notices of a cartridge"""
    widths = notice_widths(rom)
    out = []
    for i in range(NOTICE_COUNT):
        ptr = rom[NOTICE_PTRS + 2 * i] | (rom[NOTICE_PTRS + 2 * i + 1] << 8)
        w = widths[rom[NOTICE_CLASS + i]]
        chars = []
        for b in rom[ptr:ptr + 2 * w]:
            chars.append(bytes([b - NOTICE_BIAS]).decode(CODEC)
                         if 0x80 <= b <= 0xFE else " ")
        text = "".join(chars)
        out.append({"id": i, "width": w,
                    "lines": [text[:w].rstrip(), text[w:].rstrip()]})
    return out


def notice_spans(us, entries, glyphs=None):
    """notices -> (cart spans, font packet spans)

    Strings are laid end to end in the bank-01 filler, and the pointer and
    class tables rewritten to match. Accented letters take slots from $F0 in
    the order they first appear, with glyphs from the glyph source (a donor's
    notice bank, or a painted accents picture).
    """
    widths = notice_widths(us)
    by_id = dict((e["id"], e) for e in entries)
    if sorted(by_id) != list(range(NOTICE_COUNT)):
        sys.exit("notices need ids 0..%d, each exactly once" % (NOTICE_COUNT - 1))
    slot_of, font_spans = {}, []
    blob, ptrs, classes = bytearray(), bytearray(), bytearray()
    at = NOTICE_FREE[0]
    for i in range(NOTICE_COUNT):
        e = by_id[i]
        w, lines = e.get("width"), e.get("lines")
        if w not in widths:
            sys.exit("notice %d: width %r; a notice line holds %s characters"
                     % (i, w, " or ".join(str(x) for x in widths)))
        if not isinstance(lines, list) or len(lines) != 2:
            sys.exit("notice %d: \"lines\" must be two strings" % i)
        for n, line in enumerate(lines):
            if len(line) > w:
                wider = [x for x in widths if x >= len(line)]
                sys.exit('notice %d line %d is %d characters, "%s"; this notice '
                         "holds %d a line%s" % (i, n + 1, len(line), line, w,
                         "" if not wider else ", so set its width to %d" % wider[0]))
        body = bytearray()
        for ch in lines[0].ljust(w) + lines[1].ljust(w):
            try:
                code = ch.encode(CODEC)[0]
            except UnicodeEncodeError:
                sys.exit("notice %d: %r has no code in the game's character set"
                         % (i, ch))
            if 0x20 <= code <= 0x7E:
                body.append(code + NOTICE_BIAS)
                continue
            if code not in slot_of:
                if glyphs is None:
                    sys.exit("notice %d uses %r: accented letters need --donor or "
                             "--accents-from, which supply the glyph" % (i, ch))
                glyph = glyphs.notice(code)
                if glyph is None:
                    sys.exit("notice %d uses %r, which the glyph source has no "
                             "notice glyph for" % (i, ch))
                if len(slot_of) == len(NOTICE_GLYPH_SLOTS):
                    sys.exit("the notices use more than %d different accented "
                             "letters" % len(NOTICE_GLYPH_SLOTS))
                slot = NOTICE_GLYPH_SLOTS[len(slot_of)]
                slot_of[code] = slot
                font_spans.append((slot * FONT_RAW_TILE, glyph))
            body.append(slot_of[code])
        ptrs += bytes([at & 0xff, (at >> 8) & 0xff])
        classes.append(widths.index(w))
        blob += body
        at += len(body)
    if at > NOTICE_FREE[1]:
        sys.exit("the notices need %d bytes and bank 01 has %d free"
                 % (len(blob), NOTICE_FREE[1] - NOTICE_FREE[0]))
    print("in-city notices: %d, %d bytes at $01:%04X, %d accented letter%s"
          % (NOTICE_COUNT, len(blob), NOTICE_FREE[0], len(slot_of),
             "" if len(slot_of) == 1 else "s"))
    return ([(NOTICE_FREE[0], bytes(blob)), (NOTICE_PTRS, bytes(ptrs)),
             (NOTICE_CLASS, bytes(classes))], font_spans)


def _notices_for(a, us):
    """the notice cart spans and font entry a packets run asks for, if any"""
    doc = getattr(a, "notices_doc", None)
    if doc is None and getattr(a, "notices", False):
        if not a.donor:
            sys.exit("--notices takes the donor's notices and needs --donor")
        doc = read_notices(open(a.donor, "rb").read())
    if doc is None:
        return [], []
    cart, glyphs = notice_spans(us, doc, _glyph_source(a))
    return cart, ([(FONT_PACKET, FONT_TILES * FONT_RAW_TILE, glyphs)]
                  if glyphs else [])


# ── report screens: budget, evaluation, overview, events ─────────────────
# These are pictures, not text: a 2048-byte tilemap per screen over one shared
# 2bpp tile set, $09:875C, loaded by $02:A132 while a city is running ($14 =
# $00). Identified by matching live captures against every packet in the ROM:
# the set supplies 576 of the budget screen's 584 tiles, and each tilemap
# matches its screen on every cell except the ones the game fills in.
#
#   budget $0B:BF0E   evaluation $0B:C0C9   overview $0B:C29F   events $0B:C488
#
# The German and French cartridges keep the same four screens in the same
# order, so a donor's tilemaps are the four 2048-byte packets starting at the
# twin of the budget map.
#
# Why not simply take the donor's tile set: it differs on 714 of 1024 tiles,
# and the game also draws words from it at runtime -- the city category
# ("Metropolis" is tiles $197..$19D), the problem list, the game level -- at
# tile numbers fixed in the US code. The donor keeps those words elsewhere, so
# a swapped set turns "Metropolis" into a fragment. The same set is also
# loaded by the briefing screen through $03:DF77, on $14 = $0C.
#
# So an import changes only what the picture changes. Per cell: a tile used by
# that cell alone is rewritten in place; otherwise an identical existing tile
# is reused, or a free one taken. Free means blank, referenced by none of the
# four maps, not among the tiles seen written at runtime, and not on a row of
# the 16-wide sheet that holds any unreferenced artwork -- runtime word strips
# are unreferenced artwork, and their blank padding is on the same rows. The
# entries are scoped to $14 = $00, so the briefing screen keeps its tiles.
US_ROM_DEFAULT = "Sim City (U) [!].sfc"
REPORT_CHR = 0x04875C
REPORT_SCREEN = 0x00
REPORTS = (("budget", 0x05BF0E), ("evaluation", 0x05C0C9),
           ("overview", 0x05C29F), ("events", 0x05C488))
REPORT_RAMP = ((20, 20, 30), (110, 120, 150), (190, 195, 215), (250, 250, 255))
REPORT_ATTR = 0x3C00            # palette and priority; tile and flips excluded
# The runtime word strips -- the problem list and the city category -- are
# unreferenced artwork on rows $18-$1D. The event import may redraw them with
# the donor's strips, so a static label must never reuse one of their tiles
# merely because it looks the same today.
REPORT_STRIP_ROWS = frozenset(range(0x18, 0x1E))
# Tiles seen written at runtime on the four screens, from captures of each.
REPORT_RUNTIME_TILES = frozenset(
    [0x000, 0x004, 0x009, 0x00D, 0x01F, 0x030, 0x032, 0x033, 0x034, 0x03B,
     0x03C, 0x03E, 0x041, 0x042, 0x043, 0x046, 0x048, 0x051, 0x052, 0x056,
     0x059, 0x061, 0x062, 0x066, 0x069, 0x3FF]
    + list(range(0x020, 0x02A)) + list(range(0x197, 0x19E))
    # The whole small font: the event lines, the month names and every number
    # the game prints are drawn from it at runtime, so a static label that
    # happens to use a letter once must still never redraw that tile. The first
    # event import showed why -- the report import had redrawn R ($11), and
    # "Reaktorunfall" and "APR" lost it.
    + list(range(0x000, 0x060)))
# Slots written into this tile set every time it unpacks -- not by the game,
# by our own translation runtime: they are BRIEF_EXTRA_COPY's destinations,
# the accented briefing glyphs (ae oe ue ss and friends), and the set is shared
# with the briefing screen. The first report import took them as free, and
# play showed its letters replaced: the "($)" after Wert der Stadt,
# Kategorie, Schwierigkeitsgrad, one tile each in Feuerwehrstationen and
# Wasserflaechen. Their whole sheet rows are kept out, not just the nine slots.
REPORT_DYNAMIC_TILES = frozenset([0x2EC, 0x2ED, 0x2EE, 0x2EF, 0x2F0, 0x2F1,
                                  0x2F4, 0x304, 0x30B])
REPORT_DYNAMIC_ROWS = frozenset(t // 16 for t in REPORT_DYNAMIC_TILES)


def _map_words(b):
    return [b[i] | (b[i + 1] << 8) for i in range(0, len(b), 2)]


def _cell_px(chrb, e):
    """64 colour indices for one tilemap cell, flips applied"""
    t = e & 0x3ff
    g = chrb[t * 16:t * 16 + 16]
    rows = [[(1 if g[y * 2] & (0x80 >> x) else 0)
             | (2 if g[y * 2 + 1] & (0x80 >> x) else 0) for x in range(8)]
            for y in range(8)]
    if e & 0x4000:
        rows = [r[::-1] for r in rows]
    if e & 0x8000:
        rows = rows[::-1]
    return tuple(v for r in rows for v in r)


def _tile_2bpp(px):
    b = bytearray(16)
    for y in range(8):
        for x in range(8):
            v, m = px[y * 8 + x], 0x80 >> x
            if v & 1:
                b[y * 2] |= m
            if v & 2:
                b[y * 2 + 1] |= m
    return bytes(b)


def report_sources(path, eg):
    """(tile set, [(name, map words)]) for a cartridge's four report screens"""
    rom = open(path, "rb").read()
    us = open(US_ROM_DEFAULT, "rb").read()
    uchr, _ = eg.nintendo_decompress(us, REPORT_CHR)
    umaps = [(n, _map_words(eg.nintendo_decompress(us, o)[0])) for n, o in REPORTS]
    if rom == us:
        return bytes(uchr), umaps
    pk = scan_packets(rom, eg, 512)
    chr_tw = find_twin([x for x in pk if len(x[1]) == 16384], uchr)
    maps = sorted(x for x in pk if len(x[1]) == 2048)
    budget = find_twin(maps, bytes(eg.nintendo_decompress(us, REPORTS[0][1])[0]))
    if not chr_tw or not budget:
        sys.exit("no report screens found in %s" % os.path.basename(path))
    start = [o for o, _ in maps].index(budget[0])
    picked = maps[start:start + len(REPORTS)]
    print("report screens from %s: tile set $%06X, maps %s"
          % (os.path.basename(path), chr_tw[0],
             " ".join("$%06X" % o for o, _ in picked)))
    return chr_tw[2], [(n, _map_words(d)) for (n, _), (_, d) in zip(REPORTS, picked)]


# Colour attributes in a tilemap picture. A cell shows in the screen's plain
# ramp when it keeps the palette and priority it has in the US tilemap, and in
# a ramp of its own when it has others: one hue per palette, paler for the
# priority bit. So a US export is plain as it always was, a donor's shows its
# recoloured cells in colour, and an import reads the pixels and the attribute
# back -- the German evaluation alone recolours 198 cells. Plain pictures from
# before import exactly as they did.
ATTR_HUES = ((255, 64, 64), (64, 200, 64), (72, 112, 255), (232, 200, 40),
             (224, 72, 224), (40, 200, 200), (255, 144, 32), (152, 96, 255))


def _attr_ramp(attr):
    """four colours for cells with the palette/priority bits `attr` ($3C00)"""
    hue = ATTR_HUES[(attr >> 10) & 7]
    if attr & 0x2000:
        hue = tuple((x + 255) // 2 for x in hue)
    return [tuple(x * (k + 1) // 4 for x in hue) for k in range(4)]


def _attr_decoder(plain):
    """a nearest-colour lookup: RGB -> (attr, or None for the plain ramp; index)"""
    table = [(c, None, k) for k, c in enumerate(plain)]
    for pal in range(8):
        for prio in (0, 0x2000):
            at = (pal << 10) | prio
            table += [(c, at, k) for k, c in enumerate(_attr_ramp(at))]
    memo = {}

    def decode(c):
        if c not in memo:
            memo[c] = min(table, key=lambda t: sum((c[j] - t[0][j]) ** 2
                                                   for j in range(3)))[1:]
        return memo[c]
    return decode


def _decode_cell(decode, pts, where):
    """64 RGB pixels -> (64 indices, attr or None); a cell is one ramp"""
    got = [decode(q) for q in pts]
    ramps = set(at for at, _ in got)
    if len(ramps) > 1:
        sys.exit("%s is painted in more than one colour ramp" % where)
    return tuple(k for _, k in got), ramps.pop()


def cmd_reports(a):
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    eg = _lz5()
    chrb, maps = report_sources(getattr(a, "from") or a.rom, eg)
    us = open(a.rom, "rb").read()
    us_maps = dict((n, _map_words(eg.nintendo_decompress(us, o)[0])) for n, o in REPORTS)
    os.makedirs(a.out, exist_ok=True)
    for name, words in maps:
        img = PIL.Image.new("RGB", (256, 256))
        px = img.load()
        for i, e in enumerate(words):
            at = e & REPORT_ATTR
            ramp = REPORT_RAMP if at == us_maps[name][i] & REPORT_ATTR else _attr_ramp(at)
            for k, v in enumerate(_cell_px(chrb, e)):
                px[(i % 32) * 8 + k % 8, (i // 32) * 8 + k // 8] = ramp[v]
        img.save(os.path.join(a.out, name + ".png"))
        print("  %s.png" % name)
    print("report screens -> %s. Paint in the four colours only, keep text on "
          "the 8-pixel grid, and leave the areas the game fills in (numbers, "
          "the year, the problem list) empty. A cell in colour has a palette or "
          "priority of its own; keep a cell within one ramp." % a.out)


def report_spans(us, eg, painted, attrs=None):
    """{screen: 1024 cells of 64 indices} -> packet entries for the import"""
    chr_u = bytes(eg.nintendo_decompress(us, REPORT_CHR)[0])
    maps = dict((n, _map_words(eg.nintendo_decompress(us, o)[0])) for n, o in REPORTS)
    refs = {}
    for words in maps.values():
        for e in words:
            refs[e & 0x3ff] = refs.get(e & 0x3ff, 0) + 1
    blank = lambda t: len(set(chr_u[t * 16:t * 16 + 16])) <= 1
    tiles = len(chr_u) // 16
    unsafe = set(t // 16 for t in range(tiles) if t not in refs and not blank(t))
    unsafe |= REPORT_DYNAMIC_ROWS
    free = [t for t in range(tiles) if t not in refs and blank(t)
            and t not in REPORT_RUNTIME_TILES and t // 16 not in unsafe]
    reserved = set(event_glyph_slots(us, eg))
    free = [t for t in free if t not in reserved]
    attrs = attrs or {}
    # pass 1: cells whose own tile can simply be redrawn
    work, claimed = {}, {}
    for name, _ in REPORTS:
        if name not in painted:
            continue
        for i, e in enumerate(maps[name]):
            want = painted[name][i]
            attr = attrs[name][i] if name in attrs else e & REPORT_ATTR
            if want == _cell_px(chr_u, e):
                if attr != e & REPORT_ATTR:
                    work.setdefault(name, []).append((i, "attr", attr, None))
                continue
            t = e & 0x3ff
            if (refs[t] == 1 and t not in REPORT_RUNTIME_TILES
                    and t // 16 not in REPORT_DYNAMIC_ROWS
                    and not e & 0xC000):
                claimed[t] = want
                work.setdefault(name, []).append((i, "place", attr, t))
            else:
                work.setdefault(name, []).append((i, "new", attr, None))
    # pass 2: everything else reuses an untouched identical tile or takes a free one
    have = {}
    for t in range(tiles):
        if (t not in claimed and t // 16 not in REPORT_DYNAMIC_ROWS
                and (t in refs or t // 16 not in REPORT_STRIP_ROWS)):
            have.setdefault(_cell_px(chr_u, t), t)
    for t, px in claimed.items():
        have.setdefault(px, t)
    new_chr = bytearray(chr_u)
    for t, px in claimed.items():
        new_chr[t * 16:t * 16 + 16] = _tile_2bpp(px)
    taken, entries = 0, []
    for name, off in REPORTS:
        if name not in work:
            continue
        words = list(maps[name])
        n_place = n_reuse = n_new = n_attr = 0
        for i, how, attr, t in work[name]:
            if how == "attr":
                words[i] = (words[i] & ~REPORT_ATTR) | attr
                n_attr += 1
                continue
            if how == "place":
                n_place += 1
            else:
                want = painted[name][i]
                t = have.get(want)
                if t is None:
                    if taken == len(free):
                        sys.exit("the report screens need more than the %d free "
                                 "tiles" % len(free))
                    t = free[taken]
                    taken += 1
                    new_chr[t * 16:t * 16 + 16] = _tile_2bpp(want)
                    have[want] = t
                    n_new += 1
                else:
                    n_reuse += 1
            words[i] = t | attr
        print("  %-10s %3d redrawn in place, %3d reuse a tile, %3d new tiles, "
              "%3d colour only" % (name, n_place, n_reuse, n_new, n_attr))
        entries.append((off, 2048, [(0, b"".join(
            bytes([w & 0xff, w >> 8]) for w in words))], (REPORT_SCREEN,)))
    spans = [(t * 16, bytes(new_chr[t * 16:t * 16 + 16]))
             for t in range(tiles) if new_chr[t * 16:t * 16 + 16] != chr_u[t * 16:t * 16 + 16]]
    print("  report tile set: %d tiles changed, %d of %d free tiles used"
          % (len(spans), taken, len(free)))
    if spans:
        entries.insert(0, (REPORT_CHR, len(chr_u), spans, (REPORT_SCREEN,)))
    return entries


# What the game prints over the report screens is placed two ways. The number
# cells are data: a layout the number printer $02:B266 reads, from $02:B77C up
# to the word list. The donor moves some of them to suit its own labels --
# JA/NEIN and the problem percentages one column left, the overview's right
# column two right -- so the words that differ are copied. The title year's
# column is an operand in code, LDX #$004C in $02:B51F, found by the code
# around it in both cartridges; it only takes effect because recomp/bank02.cfg
# keeps that function on the interpreter, as the recompiled game carries
# operands as C constants. (The German cartridge also moves the budget's tax
# rate digits, STA $7E2B28.. in $02:A66E, two columns right. Not taken: nothing
# collides there, and it would put one more function on the interpreter.)
REPORT_LAYOUT_OPERAND = rb"\x0a\xaa\xbd(..)\xa8\xa9\xff"     # $02:B275 LDA $B77C,X
REPORT_CODE = (
    ("title year column", rb"\xa0\x50\x0c\xa2(.)\x00\xad\xfb\x01\xc9\x02\x00"),
)


def _bank02_once(rom, pattern, what, whose):
    import re
    found = list(re.finditer(pattern, rom[0x010000:0x018000], re.S))
    if len(found) != 1:
        sys.exit("%s: the %s matches %d times in bank 02" % (whose, what, len(found)))
    return found[0]


def report_layout_spans(us, donor):
    """the donor's number cells and code operands -> cart spans"""
    rom, whose = open(donor, "rb").read(), os.path.basename(donor)
    spans = []
    for what, pattern in REPORT_CODE:
        mu = _bank02_once(us, pattern, what, "the US cartridge")
        md = _bank02_once(rom, pattern, what, whose)
        moved = [g for g in range(1, mu.re.groups + 1) if mu.group(g) != md.group(g)]
        spans += [(0x010000 + mu.start(g), md.group(g)) for g in moved]
        print("  %s: %s" % (what, "%d operand%s from the donor"
                            % (len(moved), "" if len(moved) == 1 else "s")
                            if moved else "as in the US"))
    at = lambda m: _bank02(m.group(1)[0] | (m.group(1)[1] << 8))
    ulo = at(_bank02_once(us, REPORT_LAYOUT_OPERAND, "number layout", "the US cartridge"))
    dlo = at(_bank02_once(rom, REPORT_LAYOUT_OPERAND, "number layout", whose))
    uhi, dhi = _event_tables(us)[0], _event_tables(rom)[0]
    if uhi - ulo != dhi - dlo or (uhi - ulo) % 2:
        sys.exit("%s: its number layout is %d bytes, the US one %d"
                 % (whose, dhi - dlo, uhi - ulo))
    cells = (uhi - ulo) // 2
    moved = [k for k in range(0, uhi - ulo, 2)
             if us[ulo + k:ulo + k + 2] != rom[dlo + k:dlo + k + 2]]
    if len(moved) * 4 > cells:
        sys.exit("%s: %d of %d number cells differ; that is not the same layout"
                 % (whose, len(moved), cells))
    spans += [(ulo + k, bytes(rom[dlo + k:dlo + k + 2])) for k in moved]
    print("  number cells: %d of %d placed as the donor has them" % (len(moved), cells))
    return spans


def _reports_for(a, us, eg):
    """the report screen entries a packets run asks for, if any"""
    folder = getattr(a, "reports_from", None)
    if getattr(a, "reports", False):
        if not a.donor:
            sys.exit("--reports takes the donor's report screens and needs --donor")
        chrb, maps = report_sources(a.donor, eg)
        painted = dict((n, [_cell_px(chrb, e) for e in w]) for n, w in maps)
        attrs = dict((n, [e & REPORT_ATTR for e in w]) for n, w in maps)
        print("report screens from the donor:")
        entries = report_spans(us, eg, painted, attrs)
        cart = report_layout_spans(us, a.donor)
        return entries + ([(ROM_SPAN_PSEUDO, 0, cart)] if cart else [])
    if not folder:
        return []
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    painted, attrs = {}, {}
    decode = _attr_decoder(REPORT_RAMP)
    for name, off in REPORTS:
        path = os.path.join(folder, name + ".png")
        if not os.path.exists(path):
            continue
        img = PIL.Image.open(path).convert("RGB")
        if img.size != (256, 256):
            sys.exit("%s is %dx%d; a report screen is 256x256" % ((path,) + img.size))
        px = img.load()
        uw = _map_words(eg.nintendo_decompress(us, off)[0])
        painted[name], attrs[name] = [], []
        for i in range(1024):
            pts = [px[(i % 32) * 8 + k % 8, (i // 32) * 8 + k // 8] for k in range(64)]
            idx, at = _decode_cell(decode, pts, "%s cell %d,%d" % (path, i % 32, i // 32))
            painted[name].append(idx)
            attrs[name].append(uw[i] & REPORT_ATTR if at is None else at)
    if not painted:
        sys.exit("no budget/evaluation/overview/events .png in %s" % folder)
    print("report screens from %s:" % folder)
    return report_spans(us, eg, painted, attrs)


# ── map window titles ─────────────────────────────────────────────────────
# The map analysis window ("COMPREHENSIVE", "POWER GRID", ...) draws its title
# as four 32x32 sprites over tiles $100-$13F, and the game copies the strip
# for the current map into those tiles when the window opens. The strips are
# pre-rendered in the window's graphics packet $0A:D381, fourteen of them, each
# 16 tiles wide and 2 rows tall, at packet tiles 512-959.
#
# The German and French cartridges keep all fourteen at exactly the same tiles
# -- GESAMTUEBERBLICK where COMPREHENSIVE is, FEUERSCHUTZ where FIRE RADIUS
# is -- and every tile the German packet changes lies inside that range. So a
# title is translated tile for tile: no copy table, no layout.
MAPTITLE_PACKET = 0x055381         # $0A:D381, 4bpp, 1024 tiles
MAPTITLE_TILES = (512, 960)
MAPTITLE_SCREEN = 0x00


def _maptitle_source(path, eg):
    rom = open(path, "rb").read()
    us = open(US_ROM_DEFAULT, "rb").read()
    upk = bytes(eg.nintendo_decompress(us, MAPTITLE_PACKET)[0])
    if rom == us:
        return upk
    tw = find_twin(scan_packets(rom, eg, 512), upk)
    if not tw:
        sys.exit("no map window graphics found in %s" % os.path.basename(path))
    return tw[2]


def cmd_maptitles(a):
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    eg = _lz5()
    pk = _maptitle_source(getattr(a, "from") or a.rom, eg)
    lo, hi = MAPTITLE_TILES
    img = PIL.Image.new("P", (16 * 8, (hi - lo) // 16 * 8), 0)
    pal = []
    for c in LABEL_PAL:
        pal += list(c)
    img.putpalette(pal + [0] * (768 - len(pal)))
    px = img.load()
    for t in range(lo, hi):
        rows = _tile_pixels(pk[t * 32:t * 32 + 32])
        for y in range(8):
            for x in range(8):
                px[(t - lo) % 16 * 8 + x, (t - lo) // 16 * 8 + y] = rows[y][x]
    img.save(a.out)
    print("map window titles -> %s: fourteen strips, 16 tiles wide and 2 rows "
          "tall each. Keep every title inside its own strip." % a.out)


def maptitle_spans(us, eg, pk):
    """a painted or donor title sheet -> the packet entry for what changed"""
    upk = bytes(eg.nintendo_decompress(us, MAPTITLE_PACKET)[0])
    lo, hi = MAPTITLE_TILES
    spans = [(t * 32, bytes(pk[t * 32:t * 32 + 32])) for t in range(lo, hi)
             if pk[t * 32:t * 32 + 32] != upk[t * 32:t * 32 + 32]]
    print("map window titles: %d tiles changed" % len(spans))
    return [(MAPTITLE_PACKET, len(upk), spans, (MAPTITLE_SCREEN,))] if spans else []


def _maptitles_for(a, us, eg):
    if getattr(a, "maptitles", False):
        if not a.donor:
            sys.exit("--maptitles takes the donor's map titles and needs --donor")
        return maptitle_spans(us, eg, _maptitle_source(a.donor, eg))
    path = getattr(a, "maptitles_from", None)
    if not path:
        return []
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    lo, hi = MAPTITLE_TILES
    img = PIL.Image.open(path).convert("RGB")
    if img.size != (16 * 8, (hi - lo) // 16 * 8):
        sys.exit("%s is %dx%d; the title sheet is %dx%d"
                 % ((path,) + img.size + (16 * 8, (hi - lo) // 16 * 8)))
    px = img.load()
    near = lambda c: min(range(16), key=lambda k: sum((c[j] - LABEL_PAL[k][j]) ** 2 for j in range(3)))
    pk = bytearray(eg.nintendo_decompress(us, MAPTITLE_PACKET)[0])
    for t in range(lo, hi):
        rows = [[near(px[(t - lo) % 16 * 8 + x, (t - lo) // 16 * 8 + y])
                 for x in range(8)] for y in range(8)]
        pk[t * 32:t * 32 + 32] = _tile_bytes(rows)
    return maptitle_spans(us, eg, bytes(pk))


# ── the map select screen ─────────────────────────────────────────────────
# The new-city map picker (screen $04) has two English words, both pictures:
# "MAP SELECT" in its header, on BG3 from the 2bpp set $08:C4DB, and "Please
# wait..." in the preview window, on BG1 from the 4bpp set $08:DEA2. Its
# tilemaps ($0B:9BA4, $0B:A10B) are identical in the German cartridge; only
# the artwork of the tiles they use differs -- 22 tiles give LANDKARTEN and 21
# give "Bitte warten...". NEXT, OK and No. are the same in both cartridges.
#
# $08:C4DB is also the scenario selector's set, so the entries are scoped to
# the screen that unpacks them for the picker, $04.
MAPSELECT_SCREEN = 0x04
MAPSELECT_SETS = (
    # (tile set, bytes a tile, tilemap packet, tilemap pages used)
    (0x0444DB, 16, 0x05A10B, 1),      # BG3: MAP SELECT
    (0x045EA2, 32, 0x059BA4, 1),      # BG1: Please wait...
)


#
# text_tool.py mapselect exports both layers as one picture, BG3 on the left
# and BG1 on the right, each the full 32x32-cell tilemap. An import redraws
# tiles in place, so a tile the tilemap uses in several cells must be painted
# the same in all of them; the import names the cells when it is not.
MAPSELECT_CELLS = 32


def _cell_any(chrb, bpt, e):
    """64 colour indices for a cell of a 2bpp (16) or 4bpp (32) set, flips applied"""
    t = e & 0x3ff
    g = bytes(chrb[t * bpt:(t + 1) * bpt]).ljust(bpt, b"\0")
    if bpt == 16:
        rows = [[(1 if g[y * 2] & (0x80 >> x) else 0)
                 | (2 if g[y * 2 + 1] & (0x80 >> x) else 0) for x in range(8)]
                for y in range(8)]
    else:
        rows = _tile_pixels(g)
    if e & 0x4000:
        rows = [r[::-1] for r in rows]
    if e & 0x8000:
        rows = rows[::-1]
    return tuple(v for r in rows for v in r)


def _cell_pack(px, bpt, e):
    """the inverse of _cell_any: undo the cell's flips and pack the tile"""
    rows = [list(px[y * 8:(y + 1) * 8]) for y in range(8)]
    if e & 0x8000:
        rows = rows[::-1]
    if e & 0x4000:
        rows = [r[::-1] for r in rows]
    if bpt == 32:
        return _tile_bytes(rows)
    b = bytearray(16)
    for y in range(8):
        for x in range(8):
            if rows[y][x] > 3:
                return None
            if rows[y][x] & 1:
                b[y * 2] |= 0x80 >> x
            if rows[y][x] & 2:
                b[y * 2 + 1] |= 0x80 >> x
    return bytes(b)


def _nearest_pal(c, n=16, memo={}):
    key = (c, n)
    if key not in memo:
        memo[key] = min(range(n), key=lambda k: sum((c[j] - LABEL_PAL[k][j]) ** 2
                                                    for j in range(3)))
    return memo[key]


def mapselect_pictures(path, eg, us):
    """[cells per layer, 64 indices each] as a cartridge draws the map select"""
    rom = open(path, "rb").read()
    pk = None if rom == us else scan_packets(rom, eg, 512)
    out = []
    for chr_off, bpt, map_off, pages in MAPSELECT_SETS:
        uchr = bytes(eg.nintendo_decompress(us, chr_off)[0])
        umap = bytes(eg.nintendo_decompress(us, map_off)[0])
        chrb = uchr
        if pk is not None:
            tw_chr = find_twin(pk, uchr)
            tw_map = find_twin(pk, umap)
            if not tw_chr or not tw_map:
                sys.exit("no map select graphics found in %s" % os.path.basename(path))
            if tw_map[2][:2048 * pages] != umap[:2048 * pages]:
                sys.exit("the donor's map select tilemap $%06X differs from the US "
                         "one; a tile-for-tile import would misplace it" % tw_map[0])
            chrb = tw_chr[2]
        out.append([_cell_any(chrb, bpt, e) for e in _map_words(umap[:2048 * pages])])
    return out


def cmd_mapselect(a):
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    eg = _lz5()
    us = open(a.rom, "rb").read()
    layers = mapselect_pictures(getattr(a, "from") or a.rom, eg, us)
    n = MAPSELECT_CELLS
    img = PIL.Image.new("RGB", (n * 8 * len(layers), n * 8))
    px = img.load()
    for k, cells in enumerate(layers):
        for i, cell in enumerate(cells):
            for j, v in enumerate(cell):
                px[k * n * 8 + i % n * 8 + j % 8, i // n * 8 + j // 8] = LABEL_PAL[v]
    img.save(a.out)
    print("map select -> %s: BG3 (MAP SELECT, four colours) on the left, BG1 "
          "(Please wait..., sixteen) on the right. Tiles are redrawn in place, so "
          "a tile used in several cells must look the same in all of them." % a.out)


def _mapselect_from_png(path):
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    n = MAPSELECT_CELLS
    img = PIL.Image.open(path).convert("RGB")
    if img.size != (n * 8 * len(MAPSELECT_SETS), n * 8):
        sys.exit("%s is %dx%d; the map select picture is %dx%d"
                 % ((path,) + img.size + (n * 8 * len(MAPSELECT_SETS), n * 8)))
    px = img.load()
    return [[tuple(_nearest_pal(px[k * n * 8 + i % n * 8 + j % 8, i // n * 8 + j // 8])
                   for j in range(64)) for i in range(n * n)]
            for k in range(len(MAPSELECT_SETS))]


def mapselect_spans(us, eg, pictures):
    """the map select layers as pictures -> packet entries for the tiles that change"""
    entries = []
    for (chr_off, bpt, map_off, pages), cells in zip(MAPSELECT_SETS, pictures):
        uchr = bytes(eg.nintendo_decompress(us, chr_off)[0])
        umap = bytes(eg.nintendo_decompress(us, map_off)[0])
        want, first = {}, {}
        for i, e in enumerate(_map_words(umap[:2048 * pages])):
            t = e & 0x3ff
            g = _cell_pack(cells[i], bpt, e)
            if g is None:
                sys.exit("map select $%06X, cell %d,%d: a four-colour layer painted "
                         "with more than four colours" % (chr_off, i % 32, i // 32))
            if want.setdefault(t, g) != g:
                j = first[t]
                sys.exit("map select $%06X: cells %d,%d and %d,%d share tile $%03X "
                         "but are painted differently" % (chr_off, j % 32, j // 32,
                                                          i % 32, i // 32, t))
            first.setdefault(t, i)
        spans = [(t * bpt, g) for t, g in sorted(want.items())
                 if g != uchr[t * bpt:(t + 1) * bpt]]
        print("map select: %d tiles of $%06X changed" % (len(spans), chr_off))
        if spans:
            entries.append((chr_off, len(uchr), spans, (MAPSELECT_SCREEN,)))
    return entries


def _mapselect_for(a, us, eg):
    if getattr(a, "mapselect", False):
        if not a.donor:
            sys.exit("--mapselect takes the donor's map select words and needs --donor")
        return mapselect_spans(us, eg, mapselect_pictures(a.donor, eg, us))
    if getattr(a, "mapselect_from", None):
        return mapselect_spans(us, eg, _mapselect_from_png(a.mapselect_from))
    return []


# ── accented glyphs ──────────────────────────────────────────────────────
# Four glyph sets come from a donor whenever text uses letters the US fonts
# lack: the message font's sixteen accents (ACCENT_CODES), the notices' copies
# in the in-city font (code + $60), the report screens' small face that the
# event lines borrow ($270 + code), and the nine briefing tiles
# BRIEF_EXTRA_COPY names. text_tool.py accents puts all four in one picture so
# a language with no donor cartridge can paint them: three 16x8 grids of the
# codes $80-$FF, then one row of the nine briefing tiles, 2bpp in the first
# four LABEL_PAL colours. A blank cell means no glyph; orange cells are codes
# that set does not use.
ACCENT_BANDS = ("message", "notice", "report")
ACCENT_ROWS = 3 * 8 + 1


class DonorGlyphs:
    """glyphs as a donor cartridge draws them"""

    def __init__(self, path, eg):
        self.path, self.eg = path, eg
        self.ver = detect_region(path)
        self.font, self.off = font_raw(path, self.ver)
        self._report = None

    def message(self, code):
        i = code - self.off
        g = bytes(self.font[i * FONT_RAW_TILE:(i + 1) * FONT_RAW_TILE]) if i >= 0 else b""
        return g if len(g) == FONT_RAW_TILE and g != bytes(FONT_RAW_TILE) else None

    def notice(self, code):
        i = code + NOTICE_BIAS
        g = bytes(self.font[i * FONT_RAW_TILE:(i + 1) * FONT_RAW_TILE]) if i < FONT_TILES else b""
        return g if len(g) == FONT_RAW_TILE and g != bytes(FONT_RAW_TILE) else None

    def report(self, code):
        if self._report is None:
            self._report = report_sources(self.path, self.eg)[0]
        t = EVENT_DONOR_FACE + code
        g = bytes(self._report[t * 16:t * 16 + 16])
        return g if len(g) == 16 and len(set(g)) > 1 else None

    def message_pairs(self):
        return _accent_pairs(self.path, self.ver)

    def briefing(self):
        return scen_glyphs(self.path, self.ver)


class PictureGlyphs:
    """glyphs painted into a text_tool.py accents picture"""

    def __init__(self, path, us_path):
        try:
            import PIL.Image
        except ImportError:
            sys.exit("this needs Pillow: pip install Pillow")
        img = PIL.Image.open(path).convert("RGB")
        if img.size != (16 * 8, ACCENT_ROWS * 8):
            sys.exit("%s is %dx%d; the accents picture is %dx%d"
                     % ((path,) + img.size + (16 * 8, ACCENT_ROWS * 8)))
        px = img.load()

        def cell(row, col):
            pts = [px[col * 8 + x, row * 8 + y] for y in range(8) for x in range(8)]
            if all(p == PANEL_MARK for p in pts):
                return None
            flat = [_nearest_pal(p, 4) for p in pts]
            if not any(flat):
                return None
            b = bytearray(16)
            for k, v in enumerate(flat):
                if v & 1:
                    b[k // 8 * 2] |= 0x80 >> (k % 8)
                if v & 2:
                    b[k // 8 * 2 + 1] |= 0x80 >> (k % 8)
            return bytes(b)
        self.bands = [dict((c, cell(n * 8 + (c - 0x80) // 16, (c - 0x80) % 16))
                           for c in range(0x80, 0x100)) for n in range(len(ACCENT_BANDS))]
        us_raw = scen_tiles(us_path, detect_region(us_path))[0]
        self.brief = []
        for k, dst in enumerate(BRIEF_EXTRA_COPY.values()):
            g = cell(len(ACCENT_BANDS) * 8, k)
            t = dst & 0x3ff
            if g is not None and g != bytes(us_raw[t * 16:t * 16 + 16]):
                self.brief.append((t, g))

    def message(self, code):
        return self.bands[0].get(code)

    def notice(self, code):
        return self.bands[1].get(code)

    def report(self, code):
        return self.bands[2].get(code)

    def message_pairs(self):
        return [(c, self.message(c)) for c in ACCENT_CODES if self.message(c)]

    def briefing(self):
        return list(self.brief)


def _glyph_source(a, eg=None):
    """where accented glyphs come from for this run: a painted picture, the
    donor, or nowhere"""
    if getattr(a, "accents_from", None):
        return PictureGlyphs(a.accents_from, getattr(a, "us_rom", None) or
                             getattr(a, "rom", None) or US_ROM_DEFAULT)
    if getattr(a, "donor", None):
        return DonorGlyphs(a.donor, eg or _lz5())
    return None


def cmd_accents(a):
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    eg = _lz5()
    src = getattr(a, "from") or a.rom
    donor = None if open(src, "rb").read() == open(a.rom, "rb").read() else DonorGlyphs(src, eg)
    img = PIL.Image.new("RGB", (16 * 8, ACCENT_ROWS * 8), PANEL_MARK)
    px = img.load()

    def put(row, col, g):
        for y in range(8):
            for x in range(8):
                m = 0x80 >> x
                v = ((1 if g and g[y * 2] & m else 0) | (2 if g and g[y * 2 + 1] & m else 0))
                px[col * 8 + x, row * 8 + y] = LABEL_PAL[v]
    for n, band in enumerate(ACCENT_BANDS):
        for c in range(0x80, 0x100):
            if band == "message" and c not in ACCENT_CODES:
                continue
            if band == "notice" and c + NOTICE_BIAS >= 0x160:
                continue
            g = getattr(donor, band)(c) if donor else None
            put(n * 8 + (c - 0x80) // 16, (c - 0x80) % 16, g)
    us_raw = scen_tiles(a.rom, detect_region(a.rom))[0]
    given = dict(donor.briefing()) if donor else {}
    for k, dst in enumerate(BRIEF_EXTRA_COPY.values()):
        t = dst & 0x3ff
        put(len(ACCENT_BANDS) * 8, k, given.get(t, bytes(us_raw[t * 16:t * 16 + 16])))
    img.save(a.out)
    print("accents -> %s: the message font's accents, the notices' copies and the "
          "report face as grids of codes $80-$FF (row = high nibble - 8, column = "
          "low nibble), then the nine briefing tiles. Four colours; blank = none."
          % a.out)




# ── event lines and month names ──────────────────────────────────────────
# The events screen ("LAST 10 EVENTS") writes its lines at runtime. Decompiled:
#
#   02:b2ec  string N at the position of event type T:
#            position = $02:BAB9[T], offset = $02:BAD9[N] & $0FFF
#   02:b328  from $02:B8B4 + offset: each byte is a TILE, $FE ends the line
#            (the caller then draws one more line a row down), $FF the string;
#            offsets below $5E get $100 added -- those are the large-font
#            word strips, entries 0-12
#   02:b6d0  month names from $02:B708: 12 x (three tile words, $0FFF)
#
# Entries 13-15 are Easy/Medium/Hard, 16-36 the events. The small font is
# A-Z $00-$19, a-z $30-$49, digits $20-$29, space $1F, `,.'` $1C-$1E,
# `$?!"+-` $2A-$2F, `%` $4A.
#
# The German cartridge runs a different drawer: its bytes are ASCII and CP437
# drawn from a second copy of the face at tile $270 + code, and $FD breaks a
# line nine cells further left. Its strings are therefore decoded as text and
# re-encoded for the US drawer, which starts every line at column 12: 18 cells
# to column 29, two lines. (First taken as 17. The US itself writes "A deluge
# occurred!" in 18, and column 29 is paper on the US, German and French events
# screens alike.) 22 of the 24 German entries fit once re-wrapped; the two that
# do not are shortened below and reported.
#
# The German list is longer than the US one, so it moves to the $FF filler at
# $02:FCEC and the base operand at $02:B336 is repointed. Accented letters get
# glyphs from the donor's own $270 face, which draws them as the US letters
# with dots, in free report tiles below $100, because a string byte can only
# name a tile below $100.
#
# Entries 0-12 are the evaluation's word strips: the problems and the city
# category. A German-style donor draws some as artwork -- squeezed lettering
# at its tiles $320-$35F, "Megametropole" at $280 -- and the rest ("Dorf",
# "Stadt", ...) as plain text, right-aligned with spaces. The art is copied
# into the US strip tiles (report rows $18-$1D, which no map uses) and listed
# first; the text is re-encoded like the events. The threshold at $02:B32C is
# set to where the art ends, and the donor's first cell for each string type
# ($02:BAB9) is taken too -- its category starts four columns further left.
# A US-style donor keeps the US strips, threshold and cells.
#
# The base and the threshold are operands in code, and the recompiled game
# carries operands as C constants; they take effect only because
# recomp/bank02.cfg keeps $02:B328 on the interpreter (force_lle).
EVENT_BASE_OPERAND = 0x013336       # $02:B336, operand of LDA $B8B4,Y
EVENT_THRESHOLD_OPERAND = 0x01332C  # $02:B32C, operand of CPY #$005E
EVENT_POSITIONS = 0x013AB9          # $02:BAB9, first cell of each string type
EVENT_STRIPS = 13                   # entries 0-12
EVENT_LIST = 0x0138B4               # $02:B8B4
EVENT_TABLE = 0x013AD9              # $02:BAD9
EVENT_COUNT = 37
EVENT_STRIP_BYTES = 0x60
EVENT_FIRST_TEXT = 13
EVENT_FREE = (0x017CEC, 0x018000)   # $02:FCEC to the end of bank 02
EVENT_MONTHS = 0x013708             # $02:B708
EVENT_WIDTH = 18
EVENT_LEVEL_WIDTH = 6               # entries 13-15 sit in the evaluation's level field
EVENT_SLOT_COUNT = 8
EVENT_DONOR_FACE = 0x270
EVENT_PUNCT = {" ": 0x1F, ",": 0x1C, ".": 0x1D, "'": 0x1E, "$": 0x2A, "?": 0x2B,
               "!": 0x2C, '"': 0x2D, "+": 0x2E, "-": 0x2F, "%": 0x4A}
# Donor lines that do not fit two lines of 18 cells, shortened. Keyed by the
# donor's text with its line breaks collapsed.
EVENT_SHORTER = {
    "Bev\u00f6lkerung erreicht die 30,000-Marke": ["Bev\u00f6lkerung", "30,000 erreicht"],
    "Bev\u00f6lkerung erreicht die 600,000-Marke": ["Bev\u00f6lkerung", "600,000 erreicht"],
}


def _event_code(ch):
    if "A" <= ch <= "Z":
        return ord(ch) - 65
    if "a" <= ch <= "z":
        return 0x30 + ord(ch) - 97
    if "0" <= ch <= "9":
        return 0x20 + ord(ch) - 48
    return EVENT_PUNCT.get(ch)


def _event_char(t):
    if t <= 0x19:
        return chr(65 + t)
    if 0x30 <= t <= 0x49:
        return chr(97 + t - 0x30)
    if 0x20 <= t <= 0x29:
        return chr(48 + t - 0x20)
    back = dict((v, k) for k, v in EVENT_PUNCT.items())
    return back.get(t, "?")


def _bank02(addr):
    return 0x010000 + addr - 0x8000


def _event_tables(rom):
    """(list base, offset table, month table, donor face?) for a cartridge"""
    import re
    b2 = rom[0x010000:0x018000]
    us_like = re.search(rb"\x85\x79\xb9(..)\x29\xff\x00\xc9\xff\x00", b2, re.S)
    de_like = re.search(rb"\xa9\x70\x02\x85\x79\xb9(..)\x29\xff\x00\xc9\xfd\x00", b2, re.S)
    m = us_like or de_like
    tab = re.search(rb"\xb9(..)\x29\xff\x0f\xa8", b2, re.S)
    mon = re.search(rb"\xb9(..)\xc9\xff\x0f", b2, re.S)
    if not m or not tab or not mon:
        sys.exit("no event drawer found")
    w = lambda g: g[0] | (g[1] << 8)
    return (_bank02(w(m.group(1))), _bank02(w(tab.group(1))),
            _bank02(w(mon.group(1))), de_like is not None and not us_like)


def read_events(rom):
    """({id: [lines]} for entries 13-36, [12 month names]) from a cartridge"""
    base, table, months, face = _event_tables(rom)
    out = {}
    for i in range(EVENT_FIRST_TEXT, EVENT_COUNT):
        off = (rom[table + 2 * i] | (rom[table + 2 * i + 1] << 8)) & 0x0FFF
        a, text = base + off, []
        while rom[a] != 0xFF and len(text) < 120:
            b = rom[a]
            a += 1
            if b in (0xFD, 0xFE):
                text.append("\n")
            elif face:
                text.append(bytes([b]).decode(CODEC))
            else:
                text.append(_event_char(b))
        out[i] = "".join(text).split("\n")
    names, cur, a = [], "", months
    while len(names) < 12:
        wd = rom[a] | (rom[a + 1] << 8)
        a += 2
        if wd == 0x0FFF:
            names.append(cur)
            cur = ""
            continue
        t = wd & 0x3FF
        cur += (_event_char(t) if t < EVENT_DONOR_FACE
                else bytes([t - EVENT_DONOR_FACE]).decode(CODEC))
    return out, names


def _event_fit(i, lines, level_width=EVENT_LEVEL_WIDTH):
    """re-wrap to the US layout, or None if it cannot fit"""
    if i < 16:
        # a level name is one field, spaces and all: a donor right-aligns it
        text = "".join(lines)
        return [text] if len(lines) == 1 and len(text) <= level_width else None
    width, most = EVENT_WIDTH, 2
    words = " ".join(lines).split()
    wrapped = [""]
    for wd in words:
        cand = (wrapped[-1] + " " + wd).strip()
        if len(cand) <= width:
            wrapped[-1] = cand
        else:
            wrapped.append(wd)
    if len(wrapped) <= most and all(len(x) <= width for x in wrapped):
        return wrapped
    return None


def report_free_tiles(us, eg):
    """free report tiles, by the same rule report_spans uses"""
    chr_u = bytes(eg.nintendo_decompress(us, REPORT_CHR)[0])
    refs = set()
    for _, o in REPORTS:
        refs |= set(w & 0x3ff for w in _map_words(eg.nintendo_decompress(us, o)[0]))
    blank = lambda t: len(set(chr_u[t * 16:t * 16 + 16])) <= 1
    tiles = len(chr_u) // 16
    unsafe = set(t // 16 for t in range(tiles) if t not in refs and not blank(t))
    unsafe |= REPORT_DYNAMIC_ROWS
    return [t for t in range(tiles) if t not in refs and blank(t)
            and t not in REPORT_RUNTIME_TILES and t // 16 not in unsafe]


def event_glyph_slots(us, eg):
    low = [t for t in report_free_tiles(us, eg) if t < 0x100 and t not in (0xFE, 0xFF)]
    return low[:EVENT_SLOT_COUNT]


def _event_positions(rom):
    """file offset of a cartridge's first-cell table, a word per string type"""
    import re
    m = re.search(rb"\xbd(..)\x0a\xaa\x98\x29\xff\x00", rom[0x010000:0x018000], re.S)
    if not m:
        sys.exit("no word position table found")
    return _bank02(m.group(1)[0] | (m.group(1)[1] << 8))


def _event_level_width(rom):
    """the level field a cartridge's own cells give: its widest level name"""
    if not _event_tables(rom)[3]:
        return EVENT_LEVEL_WIDTH
    ev = read_events(rom)[0]
    return max([EVENT_LEVEL_WIDTH] + [len("".join(ev[i]))
                                      for i in range(EVENT_FIRST_TEXT, 16)])


def read_strips(rom):
    """entries 0-12 of a German-style cartridge as ("art", its tiles) or
    ("text", a string); None for a US-style one, whose strips stay"""
    base, table, _, face = _event_tables(rom)
    if not face:
        return None
    out = []
    for i in range(EVENT_STRIPS):
        off = (rom[table + 2 * i] | (rom[table + 2 * i + 1] << 8)) & 0x0FFF
        a, raw = base + off, []
        while rom[a] < 0xFD and len(raw) < 32:
            raw.append(rom[a])
            a += 1
        if raw and all(0x20 <= b < 0x7F and _event_code(chr(b)) is not None
                       for b in raw):
            out.append(("text", bytes(raw).decode("ascii")))
        else:
            out.append(("art", [EVENT_DONOR_FACE + b for b in raw]))
    return out


def strip_art_tiles(us, eg):
    """report tiles word strip art may take: rows $18-$1D, less any a map uses"""
    refs = set()
    for _, o in REPORTS:
        refs |= set(w & 0x3ff for w in _map_words(eg.nintendo_decompress(us, o)[0]))
    return [t for t in range(0x400) if t // 16 in REPORT_STRIP_ROWS and t not in refs]


# The word strips as a picture. text_tool.py strips draws the thirteen entries
# on a 32-cell row each, starting at the column the game starts them at: the
# problems (0-6) at the problem list's, the categories (7-12) at the category
# field's; cells a strip does not use are orange. An import makes every row
# art, pads a row that starts right of the leftmost row of its group with
# blank cells -- right alignment, as the German category does it -- and moves
# the group's column to that leftmost start.
STRIP_THRESHOLD = rb"\xc0(..)\xb0\x03\xa9\x00\x01"     # CPY #$005E at $02:B32B


def _strip_cells(rom, chrb):
    """(13 lists of 16-byte glyphs, problem column, category column) as an
    image's word drawer draws its strips"""
    import re
    base, table, _, face = _event_tables(rom)
    thr = 0
    if not face:
        m = re.search(STRIP_THRESHOLD, rom[0x010000:0x018000], re.S)
        if not m:
            sys.exit("no word drawer threshold found")
        thr = _w(m.group(1), 0)
    strips = []
    for i in range(EVENT_STRIPS):
        off = _w(rom, table + 2 * i) & 0x0FFF
        a, tiles = base + off, []
        while rom[a] < (0xFD if face else 0xFE) and len(tiles) < 32:
            tiles.append(rom[a] + (EVENT_DONOR_FACE if face else
                                   (0x100 if off < thr else 0)))
            a += 1
        strips.append([bytes(chrb[t * 16:t * 16 + 16]) for t in tiles])
    pos = _event_positions(rom)
    return strips, _w(rom, pos) % 32, _w(rom, pos + 8) % 32


def donor_strip_set(path, eg):
    """(strips, first-cell table, level width) for a German-style donor;
    None for a US-style one, whose strips stay"""
    rom = open(path, "rb").read()
    strips = read_strips(rom)
    if strips is None:
        return None
    chrb = report_sources(path, eg)[0]
    art = lambda tiles: [bytes(chrb[t * 16:t * 16 + 16]) for t in tiles]
    pos = _event_positions(rom)
    return ([(k, art(v) if k == "art" else v) for k, v in strips],
            bytes(rom[pos:pos + 32]), _event_level_width(rom))


def cmd_strips(a):
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    eg = _lz5()
    path = getattr(a, "from") or a.rom
    strips, pcol, ccol = _strip_cells(open(path, "rb").read(), report_sources(path, eg)[0])
    img = PIL.Image.new("RGB", (32 * 8, EVENT_STRIPS * 8), PANEL_MARK)
    px = img.load()
    for i, glyphs in enumerate(strips):
        col = pcol if i < 7 else ccol
        for k, g in enumerate(glyphs):
            if col + k >= 32:
                break
            for y in range(8):
                for x in range(8):
                    m = 0x80 >> x
                    v = (1 if g[y * 2] & m else 0) | (2 if g[y * 2 + 1] & m else 0)
                    px[(col + k) * 8 + x, i * 8 + y] = LABEL_PAL[v]
    img.save(a.out)
    print("word strips -> %s: the evaluation's problems (rows 0-6) from column %d "
          "and city categories (rows 7-12) from column %d, four colours. A row "
          "must be one unbroken run; the leftmost row of a group sets its column."
          % (a.out, pcol, ccol))


def _strips_from_png(path, us, eg, donor=None):
    """a painted strips picture -> a strip set, or None if it shows the US strips

    Only the problem and category columns come from the picture. The rest of
    the first-cell table, and the level field's width, are the donor's when a
    German-style donor is given -- its level names are laid out for its own
    field -- and the US ones otherwise."""
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    img = PIL.Image.open(path).convert("RGB")
    if img.size != (32 * 8, EVENT_STRIPS * 8):
        sys.exit("%s is %dx%d; the word strip picture is %dx%d"
                 % ((path,) + img.size + (32 * 8, EVENT_STRIPS * 8)))
    px = img.load()

    def cell(row, col):
        pts = [px[col * 8 + x, row * 8 + y] for y in range(8) for x in range(8)]
        if all(q == PANEL_MARK for q in pts):
            return None
        b = bytearray(16)
        for k, q in enumerate(pts):
            v = _nearest_pal(q, 4)
            if v & 1:
                b[k // 8 * 2] |= 0x80 >> (k % 8)
            if v & 2:
                b[k // 8 * 2 + 1] |= 0x80 >> (k % 8)
        return bytes(b)
    rows = []
    for i in range(EVENT_STRIPS):
        cells = [cell(i, c) for c in range(32)]
        used = [c for c in range(32) if cells[c] is not None]
        if not used:
            sys.exit("%s: word strip %d is empty" % (path, i))
        if used != list(range(used[0], used[-1] + 1)):
            sys.exit("%s: word strip %d is not one unbroken run of cells" % (path, i))
        rows.append((used[0], cells[used[0]:used[-1] + 1]))
    pcol = min(c for c, _ in rows[:7])
    ccol = min(c for c, _ in rows[7:])
    strips = [[bytes(16)] * (c - (pcol if i < 7 else ccol)) + g
              for i, (c, g) in enumerate(rows)]
    ustrips, upcol, uccol = _strip_cells(us, bytes(eg.nintendo_decompress(us, REPORT_CHR)[0]))
    drom = open(donor, "rb").read() if donor else None
    styled = drom is not None and _event_tables(drom)[3]
    if not styled and (pcol, ccol) == (upcol, uccol) and strips == ustrips:
        print("word strips: as in the US")
        return None
    if styled:
        pos = _event_positions(drom)
        cells_tab, level_width = bytearray(drom[pos:pos + 32]), _event_level_width(drom)
    else:
        cells_tab, level_width = bytearray(us[EVENT_POSITIONS:EVENT_POSITIONS + 32]), EVENT_LEVEL_WIDTH
    for t, col in ((0, pcol), (1, pcol), (2, pcol), (3, pcol), (4, ccol)):
        v = _w(cells_tab, 2 * t)
        v = v - v % 32 + col
        cells_tab[2 * t:2 * t + 2] = bytes([v & 0xFF, v >> 8])
    return [("art", g) for g in strips], bytes(cells_tab), level_width


def event_spans(us, eg, events, months, strip_set=None, glyph_source=None):
    """entries 13-36 and the month names -> (cart spans, report tile spans)

    strip_set is (strips, first-cell table, level width) from donor_strip_set()
    or _strips_from_png(): strips as ("art", [16-byte glyphs]) or ("text", str).
    None keeps the US strips. events and months None keep the US lines and
    month names byte for byte, for a build that changes only the strips."""
    slots = event_glyph_slots(us, eg)
    slot_of, glyphs = {}, []

    def tile(ch, where):
        code = _event_code(ch)
        if code is not None:
            return code
        if ch not in slot_of:
            if glyph_source is None:
                sys.exit("%s uses %r: accented letters need --donor or "
                         "--accents-from" % (where, ch))
            try:
                c = ch.encode(CODEC)[0]
            except UnicodeEncodeError:
                sys.exit("%s: %r has no code in the game's character set" % (where, ch))
            g = glyph_source.report(c)
            if g is None:
                sys.exit("%s uses %r, which the glyph source has no glyph for" % (where, ch))
            if len(slot_of) == len(slots):
                sys.exit("the event lines use more than %d accented letters"
                         % len(slots))
            slot_of[ch] = slots[len(slot_of)]
            glyphs.append((slot_of[ch] * 16, bytes(g)))
        return slot_of[ch]

    table = bytearray(us[EVENT_TABLE:EVENT_TABLE + 2 * EVENT_COUNT])

    def point(i, off):
        old = table[2 * i] | (table[2 * i + 1] << 8)
        new = (old & 0xF000) | off
        table[2 * i:2 * i + 2] = bytes([new & 0xFF, new >> 8])

    level_width, art, threshold = EVENT_LEVEL_WIDTH, [], None
    if strip_set is None:
        blob = bytearray(us[EVENT_LIST:EVENT_LIST + EVENT_STRIP_BYTES])
    else:
        strips, cells_tab, level_width = strip_set
        if us[EVENT_THRESHOLD_OPERAND - 1:EVENT_THRESHOLD_OPERAND + 2] != b"\xc0\x5e\x00":
            sys.exit("$02:B32B is not CPY #$005E; this is not the US word drawer")
        blob, room, art_of = bytearray(), strip_art_tiles(us, eg), {}
        for i, (kind, items) in enumerate(strips):
            if kind != "art":
                continue
            point(i, len(blob))
            for g in items:
                if g not in art_of:
                    if len(art_of) == len(room):
                        sys.exit("the word strips need more than the %d strip "
                                 "tiles" % len(room))
                    art_of[g] = room[len(art_of)]
                    art.append((art_of[g] * 16, g))
                blob.append(art_of[g] - 0x100)
            blob.append(0xFF)
        threshold = len(blob)
        for i, (kind, items) in enumerate(strips):
            if kind == "text":
                point(i, len(blob))
                blob += bytes(tile(ch, "word strip %d" % i) for ch in items)
                blob.append(0xFF)
        print("word strips: %d as art in %d tiles, %d as text; the art ends at $%02X"
              % (sum(k == "art" for k, _ in strips), len(art_of),
                 sum(k == "text" for k, _ in strips), threshold))
    for i in range(EVENT_FIRST_TEXT, EVENT_COUNT):
        if events is None:
            point(i, len(blob))
            a = EVENT_LIST + (_w(us, EVENT_TABLE + 2 * i) & 0x0FFF)
            while us[a] != 0xFF:
                blob.append(us[a])
                a += 1
            blob.append(0xFF)
            continue
        lines = events[i]
        width = level_width if i < 16 else EVENT_WIDTH
        most = 1 if i < 16 else 2
        if len(lines) > most or any(len(x) > width for x in lines):
            sys.exit("event entry %d %r: at most %d line%s of %d characters"
                     % (i, lines, most, "" if most == 1 else "s", width))
        point(i, len(blob))
        for n, line in enumerate(lines):
            if n:
                blob.append(0xFE)
            blob += bytes(tile(ch, "event entry %d" % i) for ch in line)
        blob.append(0xFF)
    if EVENT_FREE[0] + len(blob) > EVENT_FREE[1]:
        sys.exit("the event list needs %d bytes and bank 02 has %d free"
                 % (len(blob), EVENT_FREE[1] - EVENT_FREE[0]))
    words = bytearray(us[EVENT_MONTHS:EVENT_MONTHS + 96] if months is None else b"")
    for n, name in enumerate(months or ()):
        if len(name) > 3:
            sys.exit("month %d %r: three letters" % (n + 1, name))
        for ch in name:
            t = tile(ch, "month %d" % (n + 1)) | 0x0400
            words += bytes([t & 0xFF, t >> 8])
        words += b"\xff\x0f"
    if len(words) != 96:
        sys.exit("the month names take %d bytes; the table holds 96" % len(words))
    base = 0x8000 + EVENT_FREE[0] % 0x8000
    print("event lines: %d bytes at $02:%04X, %d accented letter%s in tiles %s"
          % (len(blob), base, len(slot_of), "" if len(slot_of) == 1 else "s",
             " ".join("$%02X" % slot_of[c] for c in slot_of)))
    cart = [(EVENT_FREE[0], bytes(blob)),
            (EVENT_BASE_OPERAND, bytes([base & 0xFF, base >> 8])),
            (EVENT_TABLE, bytes(table)), (EVENT_MONTHS, bytes(words))]
    if threshold is not None:
        cart += [(EVENT_THRESHOLD_OPERAND, bytes([threshold & 0xFF, threshold >> 8])),
                 (EVENT_POSITIONS, bytes(cells_tab))]
    return cart, glyphs + art


def _events_for(a, us, eg):
    doc = getattr(a, "events_doc", None)
    months = getattr(a, "months_doc", None)
    if doc is None and getattr(a, "events", False):
        if not a.donor:
            sys.exit("--events takes the donor's event lines and needs --donor")
        drom = open(a.donor, "rb").read()
        ev, months = read_events(drom)
        level_width = _event_level_width(drom)
        doc, bad = {}, []
        for i, lines in ev.items():
            fit = _event_fit(i, lines, level_width)
            if fit is None:
                key = " ".join(" ".join(lines).split())
                fit = EVENT_SHORTER.get(key)
                if fit is None:
                    bad.append("  entry %d %r needs %s" % (
                        i, key, "one line of %d" % level_width if i < 16
                        else "two lines of %d" % EVENT_WIDTH))
                    continue
                print("  event entry %d shortened: %s -> %s" % (i, key, " | ".join(fit)))
            doc[i] = fit
        if bad:
            print("these donor event lines do not fit and have no shorter form "
                  "in EVENT_SHORTER:")
            for line in bad:
                print(line)
            sys.exit(1)
    elif doc is not None:
        doc = dict((int(e["id"]), e["lines"]) for e in doc)
        if months is None:
            months = read_events(us)[1]
    painted = getattr(a, "strips_from", None)
    strip_set = (_strips_from_png(painted, us, eg, getattr(a, "donor", None))
                 if painted else None)
    if doc is None and strip_set is None:
        return [], []
    if not painted and getattr(a, "donor", None):
        strip_set = donor_strip_set(a.donor, eg)
    cart, glyphs = event_spans(us, eg, doc, months, strip_set, _glyph_source(a, eg))
    return cart, ([(REPORT_CHR, 16384, glyphs, (REPORT_SCREEN,))] if glyphs else [])


# ── in-city panels ───────────────────────────────────────────────────────
# The panels the icon bar opens -- GAME SPEED, OPTION, DISASTERS, INFORMATION,
# LOAD SAVE -- take their title strip and icon captions from one 4bpp sheet,
# $0A:A523 (384 tiles, unpacked to $7E8000 on $14 = $00). The window's tilemap
# is the same in every language; what changes is which sheet tile lands in
# which of its cells. $01:D729 does the copying: for page $01DF it takes a list
# through the pointer table $03:E5CF -- (cell, sheet tile) word pairs up to
# $FFFF -- and moves each tile to $7EC000 + cell * 32. On page 3 it then adds
# two 3x3 groups, cells from $01:D6F3 and nine sheet tiles from $01:D717,
# depending on $01E7.
#
# The German cartridge keeps the window and the code, redraws 175 sheet tiles
# and uses longer lists: its titles fill all twelve cells of the strip where
# the US leaves the ends of a short title undrawn. Its lists take 1426 bytes
# against the US 1362, and bank 03 has 190 free, so an import writes the table
# and the lists into the free run at $0F:9B97 and repoints the two operands
# that name them -- LDA $03E5CF,X and the LDA #$03 before PLB. Those are code,
# so recomp/bank01.cfg keeps $01:D729 on the interpreter.
#
# A page is edited as a picture. text_tool.py panels exports the five pages,
# 16x12 cells each, and page 3's nine extra tiles; cells a page leaves undrawn
# are PANEL_MARK. An import rebuilds the sheet and the lists from what the
# picture shows. $01:D729 is the only code that copies out of the unpacked
# sheet (the ASL x5 / ADC #$8000 idiom occurs twice in each cartridge, both
# there), so every sheet tile but page 3's nine extras is free to redraw; the
# 41 no US list reads are blank. Tiles that already hold a wanted glyph are
# kept as they are.
PANEL_SHEET = 0x052523
PANEL_SCREEN = 0x00
PANEL_PAGES = 5                       # $01DF 5-7 point at page 0's list
PANEL_GRID = (16, 12)
PANEL_EXTRAS = 9
PANEL_HOME = (0x079C00, 0x07A800)     # $0F:9C00, inside the $FF run $0F:9B97-$0F:A80F
PANEL_MARK = (255, 128, 0)
PANEL_READER = rb"\xc2\x30\xad\xdf\x01\x0a\xaa\xbf(..)(.)\x48\xa0\x00\x00\xe2\x20\xa9(.)\x48\xab"
PANEL_EXTRA_READS = (rb"\xbf(..)\x01\xe8\xe8\xda",                  # LDA $01D6F3,X
                     rb"\x0a\xaa\xbf(..)\x01\x0a\x0a\x0a\x0a\x0a")  # LDA $01D717,X


def _w(b, off):
    return b[off] | (b[off + 1] << 8)


def panel_layout(rom):
    """a cartridge's panel copy lists: ([(cell, sheet tile)] per page, page 3's
    eighteen extra cells, its nine extra sheet tiles, the reader's match)"""
    import re
    b1 = rom[0x8000:0x10000]
    m = re.search(PANEL_READER, b1, re.S)
    if not m:
        sys.exit("no panel copy routine found")
    tbank, lbank = m.group(2)[0], m.group(3)[0]
    base = tbank * 0x8000 + _w(m.group(1), 0) - 0x8000
    pages = []
    for k in range(PANEL_PAGES):
        a, pairs = lbank * 0x8000 + _w(rom, base + 2 * k) - 0x8000, []
        while _w(rom, a) != 0xFFFF:
            pairs.append((_w(rom, a), _w(rom, a + 2)))
            a += 4
        pages.append(pairs)
    tables = []
    for pat in PANEL_EXTRA_READS:
        hits = [x for x in re.finditer(pat, b1, re.S)
                if m.start() < x.start() < m.start() + 0x100]
        if len(hits) != 1:
            sys.exit("the panel copy routine's page 3 tables were not found")
        tables.append(_w(hits[0].group(1), 0))      # bank 01: address = file offset
    cells = [_w(rom, tables[0] + 2 * k) for k in range(2 * PANEL_EXTRAS)]
    tiles = [_w(rom, tables[1] + 2 * k) for k in range(PANEL_EXTRAS)]
    return pages, cells, tiles, m


def _panel_sheet(rom, eg, us):
    upk = bytes(eg.nintendo_decompress(us, PANEL_SHEET)[0])
    if rom == us:
        return upk
    tw = find_twin(scan_packets(rom, eg, 512), upk)
    if not tw:
        sys.exit("no panel sheet found in the donor")
    return bytes(tw[2])


def panel_pictures(path, eg, us):
    """(five pages as {cell: 32 bytes}, page 3's nine extra tiles) as a
    cartridge draws them"""
    rom = open(path, "rb").read()
    sheet = _panel_sheet(rom, eg, us)
    pages, _, extra, _ = panel_layout(rom)
    tile = lambda t: bytes(sheet[t * 32:t * 32 + 32])
    return [dict((c, tile(t)) for c, t in pg) for pg in pages], [tile(t) for t in extra]


def cmd_panels(a):
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    eg = _lz5()
    us = open(a.rom, "rb").read()
    pages, extra = panel_pictures(getattr(a, "from") or a.rom, eg, us)
    w, h = PANEL_GRID
    img = PIL.Image.new("RGB", (w * 8, h * 8 * (PANEL_PAGES + 1)), PANEL_MARK)
    px = img.load()

    def put(band, cell, b):
        rows = _tile_pixels(b)
        for y in range(8):
            for x in range(8):
                px[cell % w * 8 + x, (band * h + cell // w) * 8 + y] = LABEL_PAL[rows[y][x]]
    for k, pg in enumerate(pages):
        for cell, b in pg.items():
            put(k, cell, b)
    for k, b in enumerate(extra):
        put(PANEL_PAGES, k // 3 * w + k % 3, b)
    img.save(a.out)
    print("panels -> %s: five pages of %dx%d cells (GAME SPEED, OPTION, "
          "DISASTERS, INFORMATION, LOAD SAVE) and page 3's nine extra tiles "
          "below. Paint in the sixteen colours only; a cell left entirely "
          "orange %s is not drawn." % (a.out, w, h, PANEL_MARK))


def _panels_from_png(path):
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    w, h = PANEL_GRID
    img = PIL.Image.open(path).convert("RGB")
    if img.size != (w * 8, h * 8 * (PANEL_PAGES + 1)):
        sys.exit("%s is %dx%d; the panel sheet is %dx%d"
                 % ((path,) + img.size + (w * 8, h * 8 * (PANEL_PAGES + 1))))
    px = img.load()
    near = {}

    def index(c):
        if c not in near:
            near[c] = min(range(16), key=lambda k: sum((c[j] - LABEL_PAL[k][j]) ** 2
                                                       for j in range(3)))
        return near[c]

    def cell(band, c):
        pts = [px[c % w * 8 + x, (band * h + c // w) * 8 + y]
               for y in range(8) for x in range(8)]
        if all(p == PANEL_MARK for p in pts):
            return None
        return _tile_bytes([[index(pts[y * 8 + x]) for x in range(8)] for y in range(8)])
    pages = []
    for k in range(PANEL_PAGES):
        pg = {}
        for c in range(w * h):
            t = cell(k, c)
            if t is not None:
                pg[c] = t
        pages.append(pg)
    extra = [cell(PANEL_PAGES, k // 3 * w + k % 3) for k in range(PANEL_EXTRAS)]
    if None in extra:
        sys.exit("%s: page 3's nine extra tiles must all be painted" % path)
    return pages, extra


def panel_spans(us, eg, pages, extra):
    """pictures of the five pages and page 3's extras -> (cart spans, packet entries)"""
    sheet = bytes(eg.nintendo_decompress(us, PANEL_SHEET)[0])
    tiles = len(sheet) // 32
    upages, _, xtiles, m = panel_layout(us)
    tile = lambda b, t: bytes(b[t * 32:t * 32 + 32])
    if (all(dict((c, tile(sheet, t)) for c, t in upages[k]) == pages[k]
            for k in range(PANEL_PAGES))
            and [tile(sheet, t) for t in xtiles] == list(extra)):
        print("panels: as in the US")
        return [], []
    new = bytearray(sheet)
    for k, t in enumerate(xtiles):          # page 3's extras keep their tiles
        new[t * 32:t * 32 + 32] = extra[k]
    pool = [t for t in range(tiles) if t not in set(xtiles)]
    in_pool = set(pool)
    have = {}
    for t in range(tiles):
        if t not in in_pool:
            have.setdefault(tile(new, t), t)
    wanted, seen = [], set()
    for pg in pages:
        for c in sorted(pg):
            if c >= PANEL_GRID[0] * PANEL_GRID[1]:
                sys.exit("panel cell %d is outside the %dx%d grid" % ((c,) + PANEL_GRID))
            if pg[c] not in seen:
                seen.add(pg[c])
                wanted.append(pg[c])
    assign, used = {}, set()
    for g in wanted:
        if g in have:
            assign[g] = have[g]
    as_is = {}
    for t in pool:
        as_is.setdefault(tile(sheet, t), t)
    for g in wanted:
        if g not in assign and g in as_is:
            assign[g] = as_is[g]
            used.add(as_is[g])
    free = [t for t in pool if t not in used]
    redrawn = 0
    for g in wanted:
        if g in assign:
            continue
        if not free:
            sys.exit("the panels need more than the %d sheet tiles there are "
                     "to draw into" % len(pool))
        t = free.pop(0)
        new[t * 32:t * 32 + 32] = g
        assign[g] = t
        redrawn += 1
    home, end = PANEL_HOME
    addr = lambda off: 0x8000 + off % 0x8000
    word = lambda v: bytes([v & 0xFF, v >> 8])
    body, at, ptrs = bytearray(), {}, []
    for pg in pages:
        lst = b"".join(word(c) + word(assign[pg[c]]) for c in sorted(pg)) + b"\xff\xff"
        if lst not in at:
            at[lst] = addr(home + 2 * 8 + len(body))
            body += lst
        ptrs.append(at[lst])
    ptrs += [ptrs[0]] * 3                   # pages 5-7, as in the US
    blob = b"".join(word(v) for v in ptrs) + bytes(body)
    if home + len(blob) > end:
        sys.exit("the panel lists need %d bytes and $0F:%04X has %d"
                 % (len(blob), addr(home), end - home))
    cart = [(home, blob),
            (0x8000 + m.start(1), word(addr(home))),
            (0x8000 + m.start(2), bytes([home // 0x8000])),
            (0x8000 + m.start(3), bytes([home // 0x8000]))]
    spans = [(t * 32, tile(new, t)) for t in range(tiles) if tile(new, t) != tile(sheet, t)]
    print("panels: %d cells on five pages, %d distinct tiles (%d redrawn); lists "
          "%d bytes at $0F:%04X" % (sum(len(pg) for pg in pages), len(wanted),
                                     redrawn, len(blob), addr(home)))
    return cart, ([(PANEL_SHEET, len(sheet), spans, (PANEL_SCREEN,))] if spans else [])


def _panels_for(a, us, eg):
    """(cart spans, packet entries) for the panels a run asks for"""
    if getattr(a, "panels", False):
        if not a.donor:
            sys.exit("--panels takes the donor's panels and needs --donor")
        pages, extra = panel_pictures(a.donor, eg, us)
    elif getattr(a, "panels_from", None):
        pages, extra = _panels_from_png(a.panels_from)
    else:
        return [], []
    return panel_spans(us, eg, pages, extra)


# ── the scenario selector as a picture ───────────────────────────────────
# The selector's words -- card names, disaster lines, the heading -- are 2bpp
# tiles of $08:C4DB laid out by the 64x32 tilemap $0B:A5A1. The donor route in
# cmd_packets copies a donor's layout. text_tool.py selector exports the whole
# layer as a picture instead, with one more row below it holding the six tiles
# of Sylt's disaster line: the host draws that card, and the donor route takes
# its line from Rio's strip at row 11, columns 23-28.
#
# An import keeps every cell that still looks as it did. A changed cell takes
# an identical tile if the set has one, otherwise a redrawn tile: one whose
# cells have all changed, or a blank tile that neither this tilemap nor the
# map select screen uses ($08:C4DB is that screen's BG3 set too). A cell keeps
# its palette and priority and loses its flips. Sylt's line is written only if
# it differs from Rio's US strip -- an entry at all makes the host blank the
# card's own two lines.
SELECTOR_W = 64
MAPSELECT_BG3_MAP = 0x05A10B


def selector_pictures(path, eg, us):
    """(2048 cells of 64 indices, six Sylt tiles) as a cartridge draws the selector"""
    rom = open(path, "rb").read()
    umap = bytes(eg.nintendo_decompress(us, SELECTOR_MAP)[0])
    uchr = bytes(eg.nintendo_decompress(us, SELECTOR_CHR)[0])
    m, c = umap, uchr
    if rom != us:
        pk = scan_packets(rom, eg, 4096)
        tm, tc = find_twin(pk, umap), find_twin(pk, uchr)
        if not tm or not tc:
            sys.exit("no selector packets found in %s" % os.path.basename(path))
        m, c = tm[2], tc[2]
    words = _map_words(m)
    line = [words[SYLT_SRC_ROW * SELECTOR_W + SYLT_SRC_COL + k] & 0x3ff
            for k in range(SYLT_STRIP_LEN)]
    return ([_cell_any(c, 16, e) for e in words], [_cell_any(c, 16, t) for t in line],
            [e & 0x3C00 for e in words])


def cmd_selector(a):
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    eg = _lz5()
    us = open(a.rom, "rb").read()
    cells, sylt, attrs = selector_pictures(getattr(a, "from") or a.rom, eg, us)
    us_words = _map_words(eg.nintendo_decompress(us, SELECTOR_MAP)[0])
    rows = len(cells) // SELECTOR_W
    img = PIL.Image.new("RGB", (SELECTOR_W * 8, (rows + 1) * 8), PANEL_MARK)
    px = img.load()
    for i, cell in enumerate(cells):
        ramp = LABEL_PAL[:4] if attrs[i] == us_words[i] & 0x3C00 else _attr_ramp(attrs[i])
        for j, v in enumerate(cell):
            px[i % SELECTOR_W * 8 + j % 8, i // SELECTOR_W * 8 + j // 8] = ramp[v]
    for k, cell in enumerate(sylt):
        for j, v in enumerate(cell):
            px[k * 8 + j % 8, rows * 8 + j // 8] = LABEL_PAL[v]
    img.save(a.out)
    print("scenario selector -> %s: the 64x32 word layer in four colours, and "
          "below it the six tiles of Sylt's disaster line. The screen scrolls, "
          "so cards sit where the tilemap has them, not where they look." % a.out)


def selector_import(us, eg, path):
    """a painted selector picture -> (tilemap bytes, tile spans, Sylt spans)"""
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    umap = bytes(eg.nintendo_decompress(us, SELECTOR_MAP)[0])
    uchr = bytes(eg.nintendo_decompress(us, SELECTOR_CHR)[0])
    words = _map_words(umap)
    rows = len(words) // SELECTOR_W
    img = PIL.Image.open(path).convert("RGB")
    if img.size != (SELECTOR_W * 8, (rows + 1) * 8):
        sys.exit("%s is %dx%d; the selector picture is %dx%d"
                 % ((path,) + img.size + (SELECTOR_W * 8, (rows + 1) * 8)))
    px = img.load()
    decode = _attr_decoder(LABEL_PAL[:4])

    def cell(col, row):
        pts = [px[col * 8 + j % 8, row * 8 + j // 8] for j in range(64)]
        return _decode_cell(decode, pts, "%s cell %d,%d" % (path, col, row))
    decoded = [cell(i % SELECTOR_W, i // SELECTOR_W) for i in range(len(words))]
    cells = [d[0] for d in decoded]
    attrs = [words[i] & 0x3C00 if d[1] is None else d[1] for i, d in enumerate(decoded)]
    tiles = len(uchr) // 16
    tile = lambda b, t: bytes(b[t * 16:t * 16 + 16])
    msel = set(w & 0x3ff for w in _map_words(
        bytes(eg.nintendo_decompress(us, MAPSELECT_BG3_MAP)[0])[:2048]))
    changed = [i for i, e in enumerate(words) if cells[i] != _cell_any(uchr, 16, e)]
    chg = set(changed)
    refs = {}
    for i, e in enumerate(words):
        refs.setdefault(e & 0x3ff, set()).add(i)
    freed = sorted(t for t, cs in refs.items() if cs <= chg and t not in msel)
    blank = [t for t in range(tiles)
             if t not in refs and t not in msel and not any(tile(uchr, t))]
    pool = freed + blank
    in_pool = set(pool)
    have, as_is = {}, {}
    for t in range(tiles):
        (as_is if t in in_pool else have).setdefault(tile(uchr, t), t)
    wants = []
    for i in changed:
        g = _cell_pack(cells[i], 16, 0)
        if g is None:
            sys.exit("%s: selector cell %d,%d uses more than four colours"
                     % (path, i % SELECTOR_W, i // SELECTOR_W))
        wants.append((i, g))
    assign, used = {}, set()
    for _, g in wants:
        if g in assign:
            continue
        if g in have:
            assign[g] = have[g]
        elif g in as_is and as_is[g] not in used:
            assign[g] = as_is[g]
            used.add(as_is[g])
    free = [t for t in pool if t not in used]
    new_chr, out_map = bytearray(uchr), bytearray(umap)
    for _, g in wants:
        if g in assign:
            continue
        if not free:
            sys.exit("the selector picture needs more tiles than the %d it can "
                     "redraw" % len(pool))
        t = free.pop(0)
        new_chr[t * 16:t * 16 + 16] = g
        assign[g] = t
    for i, g in wants:
        w = attrs[i] | assign[g]
        out_map[2 * i:2 * i + 2] = bytes([w & 0xFF, w >> 8])
    recoloured = [i for i, e in enumerate(words) if i not in chg and attrs[i] != e & 0x3C00]
    for i in recoloured:                    # same picture, other palette: keep the tile
        w = (words[i] & 0xC3FF) | attrs[i]
        out_map[2 * i:2 * i + 2] = bytes([w & 0xFF, w >> 8])
    spans = [(t * 16, tile(new_chr, t)) for t in range(tiles)
             if tile(new_chr, t) != tile(uchr, t)]
    line = []
    for k in range(SYLT_STRIP_LEN):
        g = _cell_pack(cell(k, rows)[0], 16, 0)
        if g is None:
            sys.exit("%s: Sylt's line uses more than four colours" % path)
        line.append(g)
    us_line = [tile(uchr, words[SYLT_SRC_ROW * SELECTOR_W + SYLT_SRC_COL + k] & 0x3ff)
               for k in range(SYLT_STRIP_LEN)]
    sylt = [] if line == us_line else [
        ((SYLT_CARD_ROW * SYLT_CARD_W + SYLT_CARD_COL + k) * CHR_BYTES_PER_TILE, g)
        for k, g in enumerate(line)]
    print("scenario selector from %s: %d cells changed, %d recoloured, %d tiles "
          "redrawn, Sylt's line %s" % (os.path.basename(path), len(changed),
                                        len(recoloured), len(spans),
                                        "painted" if sylt else "as in the US"))
    return bytes(out_map), spans, sylt


# ── tile-for-tile sets ───────────────────────────────────────────────────
# Graphics packets whose German copy has the same length and keeps every
# word at the same tile numbers, drawn by tilemaps and code that are the same
# in both cartridges. A translation is then only the tiles that differ, as
# with the map titles. Found by pairing every US packet with its German twin
# and looking at the tiles that differ:
#
#   $07:E584  city map tiles       zone letters R and C become W and G
#   $08:E422  report screens BG1   BANK / LOANS signs, Yes/No, Go With Figures
#   $0A:FCE1  graph window         the GRAPHS title (KURVEN)
#   $0A:C4CF  gift buildings       their signs (Zoo, Casino, Stadium, Expo...)
#   $0A:81E9  city sprites         the RCI demand meter (WGI)
#   $0A:8F68  menu sprites         the RCI demand meter, second copy
#
# No screen scope: the German art is what the German game shows wherever the
# packet unpacks. text_tool.py tilesets exports each as a 16-tile-wide sheet;
# an import takes the tiles that differ from the US packet.
TILESETS = (
    # (picture, US packet, bytes a tile)
    ("citytiles.png", 0x03E584, 32),
    ("bank.png", 0x046422, 32),
    ("graphs.png", 0x057CE1, 32),
    ("gifts.png", 0x0544CF, 32),
    ("rci.png", 0x0501E9, 32),
    ("rci_menu.png", 0x050F68, 32),
)


def tileset_sheets(path, eg, us):
    """{US packet offset: that set's bytes as a cartridge has them}"""
    rom = open(path, "rb").read()
    out = {}
    for name, off, bpt in TILESETS:
        u = bytes(eg.nintendo_decompress(us, off)[0])
        if rom == us:
            out[off] = u
            continue
        tw = find_twin(scan_packets(rom, eg, 512), u)
        if not tw:
            sys.exit("no counterpart of $%06X (%s) in %s" % (off, name, os.path.basename(path)))
        out[off] = bytes(tw[2])
    return out


def cmd_tilesets(a):
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    eg = _lz5()
    us = open(a.rom, "rb").read()
    sheets = tileset_sheets(getattr(a, "from") or a.rom, eg, us)
    os.makedirs(a.out, exist_ok=True)
    for name, off, bpt in TILESETS:
        pk = sheets[off]
        n = len(pk) // bpt
        img = PIL.Image.new("RGB", (16 * 8, (n + 15) // 16 * 8), PANEL_MARK)
        px = img.load()
        for t in range(n):
            rows = _tile_pixels(pk[t * bpt:(t + 1) * bpt])
            for y in range(8):
                for x in range(8):
                    px[t % 16 * 8 + x, t // 16 * 8 + y] = LABEL_PAL[rows[y][x]]
        img.save(os.path.join(a.out, name))
        print("  %s  $%06X, %d tiles" % (name, off, n))
    print("tile sets -> %s. Sixteen colours; keep every word on the tiles it "
          "already uses, since the game places them by tile number." % a.out)


def _tilesets_from_dir(folder, eg, us):
    try:
        import PIL.Image
    except ImportError:
        sys.exit("this needs Pillow: pip install Pillow")
    out = {}
    for name, off, bpt in TILESETS:
        path = os.path.join(folder, name)
        if not os.path.exists(path):
            continue
        u = bytes(eg.nintendo_decompress(us, off)[0])
        n = len(u) // bpt
        img = PIL.Image.open(path).convert("RGB")
        if img.size != (16 * 8, (n + 15) // 16 * 8):
            sys.exit("%s is %dx%d; that sheet is %dx%d"
                     % ((path,) + img.size + (16 * 8, (n + 15) // 16 * 8)))
        px = img.load()
        pk = bytearray()
        for t in range(n):
            pk += _tile_bytes([[_nearest_pal(px[t % 16 * 8 + x, t // 16 * 8 + y])
                                for x in range(8)] for y in range(8)])
        out[off] = bytes(pk)
    if not out:
        sys.exit("no tile set pictures in %s" % folder)
    return out


def tileset_spans(us, eg, sheets):
    """{US packet offset: wanted bytes} -> packet entries for the tiles that change"""
    entries = []
    for name, off, bpt in TILESETS:
        if off not in sheets:
            continue
        u = bytes(eg.nintendo_decompress(us, off)[0])
        pk = sheets[off]
        spans = [(t * bpt, bytes(pk[t * bpt:(t + 1) * bpt])) for t in range(len(u) // bpt)
                 if pk[t * bpt:(t + 1) * bpt] != u[t * bpt:(t + 1) * bpt]]
        print("tile set %s: %d tiles changed" % (name, len(spans)))
        if spans:
            entries.append((off, len(u), spans))
    return entries


def _tilesets_for(a, us, eg):
    if getattr(a, "tilesets", False):
        if not a.donor:
            sys.exit("--tilesets takes the donor's tile sets and needs --donor")
        return tileset_spans(us, eg, tileset_sheets(a.donor, eg, us))
    if getattr(a, "tilesets_from", None):
        return tileset_spans(us, eg, _tilesets_from_dir(a.tilesets_from, eg, us))
    return []


# ── every picture at once ────────────────────────────────────────────────
# text_tool.py graphics --out DIR [--from ROM] writes each exporter's picture
# into one folder; packets and translate take --graphics-from DIR and import
# whichever of those files are present. A donor flag given as well (--reports,
# --panels, ...) still wins for its own set.
GRAPHICS = (
    # (name in the folder, exporter, the option it fills, what it is)
    ("reports", "cmd_reports", "reports_from",
     "budget, evaluation, overview and events screens, four PNGs"),
    ("maptitles.png", "cmd_maptitles", "maptitles_from", "map window titles"),
    ("labels.png", "cmd_labels", "labels_from", "the toolbar's building labels"),
    ("panels.png", "cmd_panels", "panels_from", "the in-city panels"),
    ("mapselect.png", "cmd_mapselect", "mapselect_from", "MAP SELECT and Please wait..."),
    ("strips.png", "cmd_strips", "strips_from", "the evaluation's problems and categories"),
    ("accents.png", "cmd_accents", "accents_from", "accented glyphs of four fonts"),
    ("selector.png", "cmd_selector", "selector_from",
     "scenario card names, disaster lines and Sylt's line"),
    ("tilesets", "cmd_tilesets", "tilesets_from",
     "city tiles, bank window, graph title, gift signs, RCI meters"),
)


def cmd_graphics(a):
    os.makedirs(a.out, exist_ok=True)
    src = getattr(a, "from") or a.rom
    notes = ["Pictures exported from %s by text_tool.py graphics." % os.path.basename(src),
             "Edit any of them and build with --graphics-from %s; files you" % a.out,
             "delete are simply not imported. Palette colours only, 8-pixel grid;",
             "orange cells mean 'not drawn' wherever a picture has them.", ""]
    for name, fn, _, what in GRAPHICS:
        ns = argparse.Namespace(out=os.path.join(a.out, name), rom=a.rom)
        setattr(ns, "from", getattr(a, "from"))
        if fn == "cmd_labels":
            ns.rom = src
        print("%s:" % name)
        globals()[fn](ns)
        notes.append("%-15s %s" % (name, what))
    with open(os.path.join(a.out, "README.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(notes) + "\n")
    print("all pictures -> %s" % a.out)


def _graphics_dir(a):
    """fill each *_from option from a --graphics-from folder, where a file is there"""
    folder = getattr(a, "graphics_from", None)
    if not folder:
        return
    if not os.path.isdir(folder):
        sys.exit("--graphics-from %s is not a folder" % folder)
    found = []
    for name, _, opt, _ in GRAPHICS:
        path = os.path.join(folder, name)
        if os.path.exists(path) and not getattr(a, opt, None):
            setattr(a, opt, path)
            found.append(name)
    print("graphics from %s: %s" % (folder, ", ".join(found) or "nothing"))


def write_packets(path, entries):
    """serialise the packet list and write it"""
    blob = bytearray(PACKET_MAGIC + bytes([2, 0]))
    blob += len(entries).to_bytes(2, "little")
    for ent in entries:
        src, outlen, sp = ent[:3]
        screens = ent[3] if len(ent) > 3 else ()
        if src == SYLT_CARD_PSEUDO:
            bank, addr = 0xff, 0xffff
        elif src == ROM_SPAN_PSEUDO:
            bank, addr = 0xfe, 0xffff
        else:
            bank, addr = src // 0x8000, 0x8000 + (src % 0x8000)
        blob += bytes([bank]) + addr.to_bytes(2, "little")
        blob += bytes([len(screens)]) + bytes(screens)
        blob += outlen.to_bytes(4, "little")
        blob += len(sp).to_bytes(2, "little")
        for off, data in sp:
            blob += off.to_bytes(4, "little") + len(data).to_bytes(2, "little")
            blob += data
    open(path, "wb").write(blob)
    print("  %d bytes -> %s" % (len(blob), path))


def _packets_menu_only(a, us, eg):
    """the menu alone, for a language with no cartridge to copy from"""
    art, _ = eg.nintendo_decompress(us, MENU_ART)
    field = [t.strip() for t in a.menu_text.split("|")]
    print("main menu text:")
    art_spans, cart = menu_lines_spans(us, art, dict(zip(MENU_LINE_Y, field)))
    if getattr(a, "menu_saved", None):
        sa, sc = menu_saved_spans(us, art, a.menu_saved)
        art_spans += sa
        cart += sc
    cart += _msg_cols_spans(a)
    if getattr(a, "labels_from", None):
        sp = label_spans(a.labels_from, us)
        cart += sp
        print("building labels from %s: %d tiles changed"
              % (os.path.basename(a.labels_from), len(sp)))
    ncart, nfont = _notices_for(a, us)
    ecart, echr = _events_for(a, us, eg)
    pcart, pchr = _panels_for(a, us, eg)
    ncart = ncart + ecart + pcart
    nfont = (nfont + _reports_for(a, us, eg) + _maptitles_for(a, us, eg) + echr
             + pchr + _mapselect_for(a, us, eg) + _tilesets_for(a, us, eg))
    sel = []
    if getattr(a, "selector_from", None):
        smap, sspans, sylt = selector_import(us, eg, a.selector_from)
        if smap != bytes(eg.nintendo_decompress(us, SELECTOR_MAP)[0]) or sspans:
            sel += [(SELECTOR_MAP, len(smap), [(0, smap)]),
                    (SELECTOR_CHR, 16384, sspans)]
        if sylt:
            sel.append((SYLT_CARD_PSEUDO, 0, sylt))
    write_packets(a.out, [(MENU_ART, len(art),
                           [(t * 32, d) for t, d in art_spans], MENU_SCREENS),
                          (ROM_SPAN_PSEUDO, 0, cart + ncart)] + sel + nfont)


def _msg_cols_spans(a):
    cols = getattr(a, "columns", None) or MSG_COLS_US
    sp = msg_width_spans(cols)
    if sp:
        print("message box: %d characters a line, not the US %d "
              "($01:E59F and $01:E5C3)" % (cols, MSG_COLS_US))
    return sp


def cmd_packets(a):
    eg = _lz5()
    _graphics_dir(a)
    us = open(a.rom, "rb").read()
    if not a.donor:
        # No cartridge to lift artwork from. The menu is composed from the
        # US artwork itself, so it still works; the scenario card names and
        # the building labels stay English, because they are pictures.
        for opt in ("hud", "menu", "rom_copy", "swap", "reports", "maptitles",
                    "mapselect", "events", "panels", "tilesets"):
            if getattr(a, opt, None):
                sys.exit("--%s needs --donor" % opt.replace("_", "-"))
        return _packets_menu_only(a, us, eg)
    dn = open(a.donor, "rb").read()
    # The text this packet is paired with comes from the donor, so the line
    # width it was written for does too. Defaulting to the donor's own is what
    # keeps "Dr. Wrigh / tund Du mußt" from coming back by forgetting a flag.
    if getattr(a, "columns", None) is None:
        dv = detect_region(a.donor)
        a.columns = detect_msg_cols(
            [to_text(r) for r in split_records(load_block(a.donor, dv))])
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
    # A cell whose tilemap entry is UNCHANGED can still need translating. The
    # donor is free to reuse a tile index for a different glyph, and it does:
    # San Francisco's first disaster line is tiles $0BE..$0C3 in both ROMs,
    # US "Earthquake" and French "Tremblement", at the same indices. Taking
    # art only for cells whose ENTRY changed left that line in English while
    # the second line, which the donor does move, came out French --
    # "Earthquake de terre" on the card.
    #
    # So the art of an unchanged cell is taken too, but only inside the card
    # rectangles, and only if no cell outside them shares the tile. The screen
    # around the cards -- the wood, the borders, MAP SELECT -- is left alone,
    # and so is Sylt's card, which the host composes after this packet.
    incard = set()
    for row in CARD_ROWS:
        for col in CARD_COLS:
            for y in range(CARD_H):
                for x in range(CARD_W):
                    incard.add((row + y) * 32 + col + x)
    outside = set(uw[i] & 0x3ff for i in range(len(uw)) if i not in incard)
    reused, shared = set(), set()
    for i in sorted(incard):
        if i >= len(uw) or uw[i] != dw[i]:
            continue
        t = uw[i] & 0x3ff
        if art(uchr, t) == art(dchr, t):
            continue
        (shared if t in outside else reused).add(t)
    for t in sorted(reused):
        spans.append((t * CHR_BYTES_PER_TILE, art(dchr, t)))
    if reused:
        print("  %d cells kept their entry but the donor draws them "
              "differently: tiles %s" % (len(reused),
              " ".join("$%03X" % t for t in sorted(reused))))
    if shared:
        print("  %d such tiles left alone, shared with the screen outside the "
              "cards: %s" % (len(shared),
              " ".join("$%03X" % t for t in sorted(shared))))
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
    if getattr(a, "selector_from", None):
        out_map, spans, sylt = selector_import(us, eg, a.selector_from)

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
        if getattr(a, "menu_saved", None):
            sa, sc = menu_saved_spans(us, art, a.menu_saved)
            art_spans += sa
            cart += sc
        extra.append((MENU_ART, len(art),
                      [(t * 32, d) for t, d in art_spans], MENU_SCREENS))
        rom_spans += cart

    ncart, nfont = _notices_for(a, us)
    rom_spans += ncart
    ecart, echr = _events_for(a, us, eg)
    rom_spans += ecart
    pcart, pchr = _panels_for(a, us, eg)
    rom_spans += pcart
    extra += (nfont + _reports_for(a, us, eg) + _maptitles_for(a, us, eg)
              + _mapselect_for(a, us, eg) + echr + pchr + _tilesets_for(a, us, eg))
    entries = ([(SELECTOR_MAP, len(umap), [(0, bytes(out_map))]),
                (SELECTOR_CHR, len(uchr), spans)]
               + ([(SYLT_CARD_PSEUDO, 0, sylt)] if sylt else []) + extra)
    rom_spans += _msg_cols_spans(a)
    if rom_spans:
        entries.append((ROM_SPAN_PSEUDO, 0, rom_spans))
    write_packets(a.out, entries)



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

# ── how wide the message box is ───────────────────────────────────────────
# A message record is a flat grid, not a string with line breaks: the renderer
# at $01:E59F writes a fixed number of characters, then skips to the next
# tilemap row, and the runs of spaces in a record are what pads each line out
# to the edge. So the width the text was WRITTEN for has to match the width the
# renderer draws, or every line walks.
#
#   01:e59f  LDA #$0018 / STA $79        24 characters a line
#   01:e5a7  LDA $0f0000,X ...           one character
#   01:e5c1  DEC $79 / BNE               until the line is full
#   01:e5c3  TYA / CLC / ADC #$0010      then skip 8 words to the next row
#
# 24 + 8 = 32 words, one tilemap row. The German and French cartridges run the
# same routine with `LDA #$0019` and `ADC #$000E`: 25 + 7, the same 32 words.
# US and EU are 24, French and German 25 -- measured, not assumed, by wrapping
# every record at each candidate width and counting the boundaries that fall
# inside a word.
#
# Importing German text into the US renderer without this gives exactly what
# was reported from play: "Dr. Wrigh / tund Du mußt" -- each line one character
# short, so the text slides further out of step with every row.
MSG_COLS_AT = 0x00E5A0           # the operand of LDA #$0018 at $01:E59F
MSG_GAP_AT = 0x00E5C4            # the operand of ADC #$0010 at $01:E5C3
MSG_ROW_WORDS = 32               # a tilemap row, which the two must add up to
MSG_COLS_US = 24


def msg_width_spans(cols):
    """cart spans setting the renderer's line width, or none if it is the US one"""
    if cols == MSG_COLS_US:
        return []
    if not 8 <= cols <= MSG_ROW_WORDS:
        sys.exit("a message line of %d characters cannot work: the tilemap row "
                 "is %d tiles" % (cols, MSG_ROW_WORDS))
    return [(MSG_COLS_AT, bytes([cols])),
            (MSG_GAP_AT, bytes([(MSG_ROW_WORDS - cols) * 2]))]


def detect_msg_cols(texts):
    """the width a set of records was laid out for

    Wrap each record at every candidate width and count the boundaries that
    land inside a word. The right width leaves almost none: on the US records
    24 scores 75 out of 496 and the next best is 161, and on the French ones
    25 scores 4. The residue is line-end hyphens and the records that are not
    prose at all.
    """
    best = None
    for w in range(16, MSG_ROW_WORDS + 1):
        bad = 0
        for t in texts:
            for i in range(w, len(t), w):
                if t[i - 1] not in " -" and t[i] != " ":
                    bad += 1
        if best is None or bad < best[0]:
            best = (bad, w)
    return best[1]

# ── one file per language ─────────────────────────────────────────────────
# Everything above translates from a donor CARTRIDGE, which is fine for the
# four regions Nintendo shipped and useless for a fifth. `template` writes one
# JSON holding every string the game draws as text, and `translate` turns an
# edited one back into the pair the runtime loads: the message blob and the
# packet patch.
#
# What is text and what is a picture matters here. The 53 messages, the 12
# scenario briefings and the 3 menu lines are text and live in this file. The
# scenario card names and the toolbar's building labels are pre-rendered
# artwork -- they come from a donor cartridge (--donor) or from an edited PNG
# (labels, see `labels`), and no amount of typing replaces them.
MENU_US = ("PRACTICE", "START NEW CITY", "SELECT SCENARIO")


def cmd_template(a):
    src = getattr(a, "from") or a.us_rom
    ver = detect_region(src)
    recs = split_records(load_block(src, ver))
    doc = {
        "_readme": [
            "One translation, one file. Edit the strings; leave the ids and",
            "page numbers alone -- they are what maps each string back.",
            "",
            "menu: the three main-menu option lines, top to bottom. 48",
            "characters across all three, and no line may run past the screen.",
            "Accented letters are built on the fly, so write them normally.",
            "menu_saved: the line above them while a save exists. Empty keeps",
            "the US RESUME SAVED CITY.",
            "",
            "messages: the in-game message box. It renders into a fixed-width",
            "box, so runs of spaces are LAYOUT, not padding to strip. Each",
            "line is exactly \"columns\" characters, padding included, and the",
            "renderer is set to that width when the build is made.",
            "",
            "briefings: the scenario briefing pages. title is %d characters,"
            % BRIEF_TITLE_MAX,
            "each body line %d." % BRIEF_BODY_MAX,
            "",
            "notices: the two-line boxes over the city (\"More Residential",
            "zones needed.\"). Each line holds \"width\" characters, and width",
            "is 12, 15, 19 or 23 -- a wider one gives the box a wider frame.",
            "Leading spaces are layout.",
            "",
            "events: the lines of the LAST 10 EVENTS screen, ids 16-36, at most",
            "two lines of 18 characters; ids 13-15 are the evaluation's game",
            "level, one line of 6. months: twelve three-letter names.",
            "",
            "Build it with:",
            "  python tools/text_tool.py translate --in FILE --out-prefix NAME",
            "Add --donor ROM for the accented glyphs of the message font, the",
            "scenario card names and (with --hud) the building labels. Without",
            "a donor, paint the pictures instead: text_tool.py graphics --out DIR",
            "exports every one of them, and --graphics-from DIR imports them.",
        ],
        "language": ver,
        "columns": detect_msg_cols([to_text(r) for r in recs]),
        "menu": list(MENU_US),
        "menu_saved": "",
        "messages": [{"id": i, "text": to_text(r)} for i, r in enumerate(recs)],
        "notices": read_notices(open(src, "rb").read()),
        "events": [{"id": i, "lines": l} for i, l in
                   sorted(read_events(open(src, "rb").read())[0].items())],
        "months": read_events(open(src, "rb").read())[1],
        "briefings": [{"page": p["page"], "title": p["title"],
                       "body": p["body"], "row": p["row"], "col": p["col"]}
                      for p in _briefs_doc(src)["pages"]],
    }
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
    print("template from %s (%s): %d menu lines, %d messages, %d briefings "
          "-> %s" % (os.path.basename(src), ver, len(doc["menu"]),
                     len(doc["messages"]), len(doc["briefings"]), a.out))


def _translate_briefs(doc, us_rom):
    """the JSON's briefing pages -> the strings buffer make_blob() takes

    Page numbers come from the file; the source ADDRESS each page is keyed by
    comes from the US image, because that is what the runtime hook at $00:9106
    recognises. Taking it from the file would key a donor-seeded template by
    the donor's addresses.
    """
    us = _briefs_doc(us_rom)["pages"]
    by_page = {q["page"]: q for q in doc.get("briefings", [])}
    pages, bad = [], []
    for q in us:
        o = by_page.get(q["page"], q)
        title, body = o.get("title", q["title"]), o.get("body", q["body"])
        if len(title) > BRIEF_TITLE_MAX:
            bad.append("page %d title is %d characters, the paper holds %d"
                       % (q["page"], len(title), BRIEF_TITLE_MAX))
        for line in body:
            if len(line) > BRIEF_BODY_MAX:
                bad.append("page %d line %r is %d characters, the paper holds "
                           "%d" % (q["page"], line[:20], len(line),
                                   BRIEF_BODY_MAX))
        pages.append((q["src"], title, body,
                      o.get("row", q["row"]), o.get("col", q["col"])))
    if bad:
        for m in bad[:10]:
            print("  " + m, file=sys.stderr)
        sys.exit("%d briefing line%s run off the paper"
                 % (len(bad), "" if len(bad) == 1 else "s"))
    buf = bytearray(len(pages).to_bytes(2, "little"))
    for src, title, body, brow, bcol in pages:
        buf += src.to_bytes(4, "little") + bytes([brow & 0xff, bcol & 0xff])
        tb = title.encode("latin-1", "replace")[:BRIEF_TITLE_MAX]
        buf += bytes([len(tb)]) + tb
        buf += bytes([min(len(body), 255)])
        for line in body[:255]:
            lb = line.encode("latin-1", "replace")[:BRIEF_BODY_MAX]
            buf += bytes([len(lb)]) + lb
    return bytes(buf), len(pages)


def cmd_translate(a):
    _graphics_dir(a)
    doc = json.load(open(getattr(a, "in"), encoding="utf-8"))
    lang = doc.get("language", "xx")
    print("translating %s (%s)" % (os.path.basename(getattr(a, "in")), lang))

    entries = sorted(doc["messages"], key=lambda e: e["id"])
    if [e["id"] for e in entries] != list(range(len(entries))):
        sys.exit("message ids must be 0..n-1 with none missing or repeated")
    recs, bad = [], []
    for e in entries:
        try:
            recs.append(from_text(e["text"]))
        except UnicodeEncodeError as ex:
            bad.append((e["id"], e["text"][ex.start:ex.end]))
    if bad:
        for i, ch in bad[:10]:
            print("  message %d: no glyph for %r" % (i, ch), file=sys.stderr)
        sys.exit("%d message%s use characters the game cannot draw. The "
                 "accented ones need --donor, which lifts their glyphs out of "
                 "a cartridge that has them."
                 % (len(bad), "" if len(bad) == 1 else "s"))

    strings, npages = _translate_briefs(doc, a.us_rom)
    glyphs, sglyphs = [], []
    source = _glyph_source(a)
    if source:
        pairs = source.message_pairs()
        glyphs = [(ACCENT_SLOT_BASE + k, g) for k, (_, g) in enumerate(pairs)]
        table = dict((code, ACCENT_SLOT_BASE + k) for k, (code, _) in enumerate(pairs))
        recs = [bytes(table.get(b, b) for b in r) for r in recs]
        sglyphs = source.briefing()
    elif any(ord(c) > 126 for e in entries for c in e["text"]):
        print("  note: no --donor or --accents-from, so the message font keeps "
              "the US glyphs")

    blob = make_blob(recs, (), None, glyphs, strings, sglyphs, None, None)
    open(a.out_prefix + ".bin", "wb").write(blob)
    print("  %d messages, %d briefing pages, %d accent glyphs -> %s.bin"
          % (len(recs), npages, len(glyphs), a.out_prefix))

    menu = doc.get("menu") or list(MENU_US)
    if len(menu) != len(MENU_LINE_Y):
        sys.exit("menu needs exactly %d lines, top to bottom; the file has %d"
                 % (len(MENU_LINE_Y), len(menu)))
    cols = doc.get("columns") or detect_msg_cols([e["text"] for e in entries])
    ns = argparse.Namespace(
        rom=a.us_rom, donor=a.donor, out=a.out_prefix + "_selector.scpk",
        labels_from=a.labels_from, menu_text="|".join(menu), menu=False,
        hud=a.hud, rom_copy=None, swap=None, columns=cols,
        menu_saved=doc.get("menu_saved") or None, notices=False,
        notices_doc=doc.get("notices"), reports=False,
        reports_from=a.reports_from, maptitles=False,
        maptitles_from=a.maptitles_from, panels=False,
        panels_from=a.panels_from, mapselect=False,
        mapselect_from=a.mapselect_from, accents_from=a.accents_from,
        strips_from=a.strips_from, selector_from=a.selector_from,
        tilesets=False, tilesets_from=a.tilesets_from,
        events=False,
        events_doc=doc.get("events"), months_doc=doc.get("months"))
    cmd_packets(ns)

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
    pc.add_argument("--donor", help="a translated ROM, for the scenario card "
                    "names and the building labels. Without it only the menu "
                    "and any --rom-copy/--swap are written")
    pc.add_argument("--out", required=True)
    pc.add_argument("--labels-from", metavar="PNG",
                    help="an edited building-label image (text_tool.py labels)")
    pc.add_argument("--menu-text", metavar="TOP|MIDDLE|BOTTOM",
                    help="the main menu's three option lines, e.g. "
                         "\"UBUNGSSPIEL|NEUE STADT|SCHAUPLATZE\". Leave a "
                         "field empty to keep the US wording. 36 characters "
                         "in all, and any three splits of that; accented "
                         "letters are built on the fly")
    pc.add_argument("--events", action="store_true",
                    help="take the event lines and month names from the donor")
    pc.add_argument("--tilesets", action="store_true",
                    help="take the tile-for-tile sets from the donor: city zone "
                         "letters, bank window, graph title, gift signs, RCI meters")
    pc.add_argument("--tilesets-from", metavar="DIR",
                    help="edited tile set sheets from text_tool.py tilesets")
    pc.add_argument("--graphics-from", metavar="DIR",
                    help="a folder from text_tool.py graphics: every picture in "
                         "it is imported")
    pc.add_argument("--selector-from", metavar="PNG",
                    help="a painted scenario selector from text_tool.py selector")
    pc.add_argument("--strips-from", metavar="PNG",
                    help="painted evaluation word strips from text_tool.py strips")
    pc.add_argument("--mapselect-from", metavar="PNG",
                    help="an edited map select picture from text_tool.py mapselect")
    pc.add_argument("--accents-from", metavar="PNG",
                    help="painted accented glyphs from text_tool.py accents, "
                         "instead of the donor's")
    pc.add_argument("--mapselect", action="store_true",
                    help="take the map select header and \"Please wait...\" "
                         "from the donor")
    pc.add_argument("--panels", action="store_true",
                    help="take the in-city panels (titles and icon captions) "
                         "from the donor")
    pc.add_argument("--panels-from", metavar="PNG",
                    help="an edited panel sheet from text_tool.py panels")
    pc.add_argument("--maptitles", action="store_true",
                    help="take the map window titles from the donor")
    pc.add_argument("--maptitles-from", metavar="PNG",
                    help="an edited title sheet from text_tool.py maptitles")
    pc.add_argument("--reports", action="store_true",
                    help="take the budget, evaluation, overview and events "
                         "screens from the donor")
    pc.add_argument("--reports-from", metavar="DIR",
                    help="painted report screens from text_tool.py reports")
    pc.add_argument("--notices", action="store_true",
                    help="take the in-city notices from the donor: text, box "
                         "widths and accented letters")
    pc.add_argument("--menu-saved", metavar="TEXT",
                    help="the saved-game line above the three options, shown "
                         "only while a save exists, for example GESPEICHERTE "
                         "STADT; needs --menu-text")
    pc.add_argument("--menu", action="store_true",
                    help="RETRACTED: relocates donor records and loses the "
                         "logo. Use --menu-text")
    pc.add_argument("--hud", action="store_true",
                    help="take the main map's building labels from the donor")
    pc.add_argument("--rom-copy", action="append", metavar="LO-HI",
                    help="lay the donor's bytes over this cart-image range; "
                         "repeatable")
    pc.add_argument("--columns", type=int, metavar="N",
                    help="the line width the message text is written for. The "
                         "US renderer draws %d; German and French text is laid "
                         "out for 25 and needs it said, or every line walks"
                         % MSG_COLS_US)
    pc.add_argument("--swap", action="append", metavar="ADDR",
                    help="take this whole US packet from the donor "
                         "(a file offset, e.g. 0x04A571); repeatable")
    mt = sub.add_parser("maptitles", help="the map window titles as a PNG to paint")
    mt.set_defaults(fn=cmd_maptitles)
    mt.add_argument("--out", required=True)
    mt.add_argument("--rom", default="Sim City (U) [!].sfc")
    mt.add_argument("--from", metavar="ROM",
                    help="export this cartridge's titles instead")

    ts = sub.add_parser("tilesets", help="the tile-for-tile sets as PNGs to paint")
    ts.set_defaults(fn=cmd_tilesets)
    ts.add_argument("--out", required=True, metavar="DIR")
    ts.add_argument("--rom", default="Sim City (U) [!].sfc")
    ts.add_argument("--from", metavar="ROM",
                    help="export this cartridge's sets instead")

    gx = sub.add_parser("graphics", help="every translatable picture into one folder")
    gx.set_defaults(fn=cmd_graphics)
    gx.add_argument("--out", required=True, metavar="DIR")
    gx.add_argument("--rom", default="Sim City (U) [!].sfc")
    gx.add_argument("--from", metavar="ROM",
                    help="export this cartridge's pictures instead")

    sl = sub.add_parser("selector", help="the scenario selector's words as a PNG to paint")
    sl.set_defaults(fn=cmd_selector)
    sl.add_argument("--out", required=True)
    sl.add_argument("--rom", default="Sim City (U) [!].sfc")
    sl.add_argument("--from", metavar="ROM",
                    help="export this cartridge's selector instead")

    st = sub.add_parser("strips", help="the evaluation's word strips as a PNG to paint")
    st.set_defaults(fn=cmd_strips)
    st.add_argument("--out", required=True)
    st.add_argument("--rom", default="Sim City (U) [!].sfc")
    st.add_argument("--from", metavar="ROM",
                    help="export this cartridge's strips instead")

    ms = sub.add_parser("mapselect", help="the map select words as a PNG to paint")
    ms.set_defaults(fn=cmd_mapselect)
    ms.add_argument("--out", required=True)
    ms.add_argument("--rom", default="Sim City (U) [!].sfc")
    ms.add_argument("--from", metavar="ROM",
                    help="export this cartridge's map select instead")

    ac = sub.add_parser("accents", help="the accented glyph sets as a PNG to paint")
    ac.set_defaults(fn=cmd_accents)
    ac.add_argument("--out", required=True)
    ac.add_argument("--rom", default="Sim City (U) [!].sfc")
    ac.add_argument("--from", metavar="ROM",
                    help="export this cartridge's glyphs instead")

    pn = sub.add_parser("panels", help="the in-city panels as a PNG to paint")
    pn.set_defaults(fn=cmd_panels)
    pn.add_argument("--out", required=True)
    pn.add_argument("--rom", default="Sim City (U) [!].sfc")
    pn.add_argument("--from", metavar="ROM",
                    help="export this cartridge's panels instead")

    rp = sub.add_parser("reports", help="budget/evaluation/overview/events "
                        "screens as PNGs to paint")
    rp.set_defaults(fn=cmd_reports)
    rp.add_argument("--out", required=True)
    rp.add_argument("--rom", default="Sim City (U) [!].sfc")
    rp.add_argument("--from", metavar="ROM",
                    help="export this cartridge's screens instead (German, French)")

    tp = sub.add_parser("template", help="one JSON holding every string")
    tp.set_defaults(fn=cmd_template)
    tp.add_argument("--out", required=True)
    tp.add_argument("--from", metavar="ROM",
                    help="seed the strings from this ROM (default: the US one)")
    tp.add_argument("--us-rom", default="Sim City (U) [!].sfc")

    tr = sub.add_parser("translate", help="an edited template -> a runnable "
                        "blob and packet")
    tr.set_defaults(fn=cmd_translate)
    tr.add_argument("--in", required=True)
    tr.add_argument("--out-prefix", required=True,
                    help="writes PREFIX.bin and PREFIX_selector.scpk")
    tr.add_argument("--us-rom", default="Sim City (U) [!].sfc")
    tr.add_argument("--donor", help="lift the message font's accented glyphs "
                    "and the scenario card names from this cartridge")
    tr.add_argument("--hud", action="store_true",
                    help="also take the building labels from the donor")
    tr.add_argument("--maptitles-from", metavar="PNG",
                    help="an edited map title sheet (text_tool.py maptitles)")
    tr.add_argument("--tilesets-from", metavar="DIR",
                    help="edited tile set sheets (text_tool.py tilesets)")
    tr.add_argument("--graphics-from", metavar="DIR",
                    help="a folder from text_tool.py graphics: every picture in "
                         "it is imported")
    tr.add_argument("--selector-from", metavar="PNG",
                    help="a painted scenario selector (text_tool.py selector)")
    tr.add_argument("--strips-from", metavar="PNG",
                    help="painted evaluation word strips (text_tool.py strips)")
    tr.add_argument("--mapselect-from", metavar="PNG",
                    help="an edited map select picture (text_tool.py mapselect)")
    tr.add_argument("--accents-from", metavar="PNG",
                    help="painted accented glyphs (text_tool.py accents), instead "
                         "of --donor's")
    tr.add_argument("--panels-from", metavar="PNG",
                    help="an edited panel sheet (text_tool.py panels)")
    tr.add_argument("--reports-from", metavar="DIR",
                    help="painted report screens (text_tool.py reports)")
    tr.add_argument("--labels-from", metavar="PNG",
                    help="an edited building-label image (text_tool.py labels)")

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
