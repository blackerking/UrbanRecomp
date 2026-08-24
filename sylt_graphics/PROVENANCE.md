# Sylt assets — where they came from

## The map

**Sylt map by lytron**, released 25 October 2014.

`Sim City Sylt.ips` and the `sylt_map.bin` extracted from it are lytron's work,
included here **with the author's written permission**. Please keep this credit
with the files; if you fork this repo and do not have that permission yourself,
remove them and regenerate from your own copy of the patch.

The patch as shipped repoints scenario index 5 — Rio — at a new map, so
applying it plainly *replaces* Rio. This project does not apply it. Instead
`tools/make_sylt_map.py` decompresses the map out of the patch, and the host
writes it into WRAM at run time, only when the ninth entry is chosen. That
keeps Rio, keeps the practice map at index 8, and leaves the ROM untouched —
which matters, because `src/main.c` fingerprints the ROM to decide it is the US
image, and that flag gates the host map renderer, `SC_FIBER`, the cursor-cadence
patch and the view fix.

Regenerate with:

    python tools/make_sylt_map.py "sylt_graphics/Sim City Sylt.ips"

## What is deliberately NOT here

`Sim City Rio on its own Bank.ips` is **not committed**. It carries 7605 bytes
that decompress to exactly the same 10068 bytes as Rio's map in the ROM, so it
is a verbatim copy of copyrighted ROM data — something no third-party
permission covers. This host never relocates Rio, so it is not needed.

## The card

`sylt_card_preview.png` is the card artwork and is the source of record;
`Untitled.jpg` is the earlier hand-drawn version it came from. `sylt_card.bin`
is generated:

    python tools/make_sylt_card.py sylt_graphics/sylt_card_preview.png

The tool writes its check render as `<source>_check.png`, which is gitignored —
the preview doubles as a re-importable source once touched up by hand, so
overwriting it would destroy the edit.
