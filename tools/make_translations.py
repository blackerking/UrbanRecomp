#!/usr/bin/env python3
"""Build the German or French translation files from your own cartridges.

    python tools/make_translations.py de             # or fr
    python tools/make_translations.py de --out DIR   # DIR = where the game is

Needs the US ROM and the German (or French) ROM in the current folder or next
to this tools/ folder, under any file names -- they are found by their
contents (tools/find_rom.py). Needs Python 3.9+ and Pillow
(`pip install pillow`).

Writes translation_<de|fr>.bin and translation_<de|fr>_selector.scpk. With
both next to the game, the launcher's language setting offers the language.
The files carry text and artwork from your cartridge: they are for your own
use and not to be passed on.

These are the exact commands the shipped German and French builds were made
with; running them reproduces those files byte for byte.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from find_rom import find_rom  # noqa: E402

RECIPES = {
    "de": {"menu": "ÜBUNGSSPIEL|NEUE STADT|SCHAUPLÄTZE", "saved": "GESPEICHERTE STADT"},
    "fr": {"menu": "ENTRAINE-TOI|NOUVELLE CITE|CHOISIS SCENARIO", "saved": None},
}
# Everything the donor cartridge can supply beyond the message text.
DONOR_PARTS = ["--hud", "--events", "--cities", "--saveload", "--tilesets",
               "--mapselect", "--panels", "--maptitles", "--reports", "--notices"]


def run(args):
    cmd = [sys.executable, os.path.join(HERE, "text_tool.py")] + args
    print("> text_tool.py " + " ".join(args[:1]), flush=True)
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit("text_tool.py %s failed (exit %d)" % (args[0], r.returncode))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("lang", choices=sorted(RECIPES))
    ap.add_argument("--out", default=".", help="folder to write into (default: here)")
    a = ap.parse_args()

    us = find_rom("us")
    donor = find_rom(a.lang)
    recipe = RECIPES[a.lang]
    os.makedirs(a.out, exist_ok=True)
    blob = os.path.join(a.out, "translation_%s.bin" % a.lang)
    packets = os.path.join(a.out, "translation_%s_selector.scpk" % a.lang)

    run(["import", "--donor", donor, "--us-rom", us, "--briefs", "--out", blob])
    args = ["packets", "--rom", us, "--donor", donor] + DONOR_PARTS + [
        "--menu-text", recipe["menu"]]
    if recipe["saved"]:
        args += ["--menu-saved", recipe["saved"]]
    run(args + ["--out", packets])
    print("\nwritten:\n  %s\n  %s" % (blob, packets))


if __name__ == "__main__":
    main()
