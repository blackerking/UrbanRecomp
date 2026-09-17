#!/usr/bin/env python3
"""Find a ROM by its contents, whatever the file is called.

The project ships no ROM and assumes no file name. Tools that need the US
image, or a regional one as a donor, look for it here: every .sfc/.smc file in
the repository root and the working directory is hashed (FNV-1a 32 over the
whole file, as src/main.c does) and matched against the five known images.

    python tools/find_rom.py us        # prints the path, or fails
    python tools/find_rom.py --list    # what was found

From Python:

    from find_rom import find_rom
    us = find_rom("us")
"""
import os
import sys

# FNV-1a 32 of the unheadered 512 KB images; see docs/REGIONS.md.
REGIONS = {
    "us": 0xEC01686A,
    "eu": 0xB76B1A0D,
    "fr": 0xE1F99069,
    "de": 0xAECA7623,
    "jp": 0xCCB8C347,
}

_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
_cache = None


def fnv1a32(data):
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def _candidates():
    seen, out = set(), []
    for d in (os.getcwd(), _ROOT):
        try:
            names = sorted(os.listdir(d))
        except OSError:
            continue
        for n in names:
            p = os.path.join(d, n)
            if n.lower().endswith((".sfc", ".smc")) and os.path.isfile(p):
                key = os.path.normcase(os.path.abspath(p))
                if key not in seen:
                    seen.add(key)
                    out.append(p)
    return out


def scan():
    """{region: path} for every known image found."""
    global _cache
    if _cache is None:
        _cache = {}
        by_hash = {v: k for k, v in REGIONS.items()}
        for p in _candidates():
            if os.path.getsize(p) != 0x80000:
                continue
            with open(p, "rb") as f:
                region = by_hash.get(fnv1a32(f.read()))
            if region and region not in _cache:
                _cache[region] = p
    return _cache


def find_rom(region="us", required=True):
    path = scan().get(region)
    if path is None and required:
        sys.exit("no %s ROM found: put your own copy (any file name, .sfc or .smc) "
                 "in %s, or pass its path" % (region.upper(), _ROOT))
    return path


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--list":
        for k, v in sorted(scan().items()):
            print("%s  %s" % (k, v))
    else:
        print(find_rom(sys.argv[1] if len(sys.argv) > 1 else "us"))
