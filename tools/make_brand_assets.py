#!/usr/bin/env python3
"""Derive the launcher box art and the window/executable icons from the logo.

    python tools/make_brand_assets.py

assets/urbanrecomp_logo.png is the master. The files written next to it are
committed, so a build needs neither Python nor Pillow:

    assets/boxart.jpg       launcher box art (staged as assets/img/boxart.jpg)
    assets/icon.png         window icon, read at runtime (assets/img/icon.png)
    assets/urbanrecomp.ico  embedded in the Windows executables (src/urbanrecomp.rc)
"""
import os

from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ASSETS = os.path.join(ROOT, "assets")


def main():
    logo = Image.open(os.path.join(ASSETS, "urbanrecomp_logo.png")).convert("RGB")
    logo.resize((512, 512), Image.LANCZOS).save(
        os.path.join(ASSETS, "boxart.jpg"), quality=90, optimize=True)
    logo.resize((128, 128), Image.LANCZOS).convert("RGBA").save(
        os.path.join(ASSETS, "icon.png"), optimize=True)
    logo.convert("RGBA").save(
        os.path.join(ASSETS, "urbanrecomp.ico"),
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    for n in ("boxart.jpg", "icon.png", "urbanrecomp.ico"):
        print("%-16s %7d bytes" % (n, os.path.getsize(os.path.join(ASSETS, n))))


if __name__ == "__main__":
    main()
