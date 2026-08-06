#!/usr/bin/env python3
"""Exploratory extraction of additional graphics regions identified by
community RAM-mapping notes (monster/disaster sprite, UFO sprite) -- see
docs/REFERENCE_third_party_optimization_patch.md and the ROM map for
attribution. Candidate offsets found by scanning near the approximate
addresses for valid LC_LZ5 packets, since the original notes were
secondhand ("I guess the program means...").
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from extract_graphics import nintendo_decompress, render_tileset, tileset_reduce

ROM = os.path.join(os.path.dirname(__file__), "..", "simcity.sfc")
OUT = os.path.join(os.path.dirname(__file__), "..", "extracted_assets")

CANDIDATES = {
    "monster_candidate_1fa98": 0x1FA98,
    "monster_candidate_1fb7c": 0x1FB7C,
    "ufo_candidate_29e35": 0x29E35,
    "ufo_candidate_2a142": 0x2A142,
}

if __name__ == "__main__":
    rom = open(ROM, "rb").read()
    os.makedirs(OUT, exist_ok=True)
    for name, off in CANDIDATES.items():
        d, end = nintendo_decompress(rom, off)
        open(os.path.join(OUT, name + ".bin"), "wb").write(d)
        reduced = tileset_reduce(d)
        render_tileset(reduced, columns=16).save(os.path.join(OUT, name + "_1bpp.png"))
        render_tileset(d, columns=16).save(os.path.join(OUT, name + "_raw2bpp.png"))
        print("%s: %d bytes (%d compressed) -> %s_1bpp.png / _raw2bpp.png" %
              (name, len(d), end - off, name))
