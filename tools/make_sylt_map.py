#!/usr/bin/env python3
"""Extract the Sylt map from its IPS patch into a runtime asset.

The patch places a compressed map at file 0x080000 and repoints scenario index
5 -- Rio -- at it. Applying it as-is would therefore REPLACE Rio, and the
companion "Rio on its own Bank" patch exists to move Rio out of the way first.

This host does neither, for two reasons:

  * patching the ROM changes its FNV, and src/main.c matches that exactly to
    decide the image is the US one. That flag gates the host map renderer,
    SC_FIBER, the cursor-cadence patch and the view fix, so a patched ROM
    silently loses all four.
  * scenario index 8 is the PRACTICE map, not a spare slot, so the ninth entry
    cannot simply take it over.

Instead the map is decompressed here and written into WRAM at run time, only
when the ninth entry was actually chosen -- see sylt_load_map() in main.c. Rio
keeps its slot, practice keeps its map, and the ROM is untouched.

Output is the DECOMPRESSED intermediate, which is what 03:ce2e leaves at
$7E8000 for 03:ce5e's JSR $d15f to unpack. Handing the game that form means its
own unpacker still does the work.

Usage:
    python tools/make_sylt_map.py "sylt_graphics/sylt.ips"
"""
import importlib.util
import os
import sys


def load_decompressor():
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location(
        "eg", os.path.join(here, "extract_graphics.py"))
    mod = importlib.util.module_from_spec(spec)
    argv, sys.argv = sys.argv, ["make_sylt_map"]
    try:
        spec.loader.exec_module(mod)
    except SystemExit:
        pass
    finally:
        sys.argv = argv
    return mod


def ips_records(path):
    d = open(path, "rb").read()
    if d[:5] != b"PATCH":
        raise SystemExit("%s is not an IPS patch" % path)
    i, out = 5, []
    while d[i:i + 3] != b"EOF":
        off = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2]
        i += 3
        size = (d[i] << 8) | d[i + 1]
        i += 2
        if size == 0:                      # RLE run
            run = (d[i] << 8) | d[i + 1]
            i += 2
            out.append((off, bytes([d[i]]) * run))
            i += 1
        else:
            out.append((off, d[i:i + size]))
            i += size
    return out


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    src = sys.argv[1]
    eg = load_decompressor()

    blob = None
    for off, data in ips_records(src):
        if off == 0x080000:                # $108000 in LoROM
            blob = data
    if blob is None:
        raise SystemExit("no record at file 0x080000 -- is this the map patch?")

    raw, used = eg.nintendo_decompress(blob, 0)
    dst = os.path.join(os.path.dirname(src) or ".", "sylt_map.bin")
    open(dst, "wb").write(raw)
    print("%s -> %s  (%d compressed -> %d bytes)"
          % (os.path.basename(src), dst, used, len(raw)))


main()
