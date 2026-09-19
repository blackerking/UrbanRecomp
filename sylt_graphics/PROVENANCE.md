# Sylt assets — where they came from

## The map

**Sylt map by lytron**, released 25 October 2014.

`sylt.ips` and the `sylt_map.bin` extracted from it are lytron's work,
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

    python tools/make_sylt_map.py "sylt_graphics/sylt.ips"

## What is deliberately NOT here

The "Rio on its own Bank" patch is **not committed**. It carries 7605 bytes
that decompress to exactly the same 10068 bytes as Rio's map in the ROM, so it
is a verbatim copy of copyrighted ROM data — something no third-party
permission covers. This host never relocates Rio, so it is not needed.

## The card

The card artwork is this project's own.

| file | what |
|---|---|
| `sylt_card.png` | the card as it ships -- the source of record |
| `sylt_card_sketch.jpg` | the hand drawing the card was made from |
| `sylt_card.bin` | 2bpp tiles the game loads, generated from `sylt_card.png` |

Regenerate with:

    python tools/make_sylt_card.py sylt_graphics/sylt_card.png

The tool also writes a check render, `sylt_card_check.png`, which is
gitignored. It is named after the source rather than overwriting it, because
the card is touched up by hand and re-imported.

## Everything in this folder

| file | from | used by |
|---|---|---|
| `sylt.ips` | lytron, with permission | `tools/make_sylt_map.py` |
| `sylt_map.bin` | generated from `sylt.ips` | the game, at run time |
| `sylt_card.png`, `sylt_card_sketch.jpg` | this project | `tools/make_sylt_card.py` |
| `sylt_card.bin` | generated from `sylt_card.png` | the game, at run time |
