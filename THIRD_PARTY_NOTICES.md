# Third-party notices

Urban Recomp itself is under the MIT licence (`LICENSE`). A program built from
this repository also contains, or ships next to, the following.

## snesrecomp -- PolyForm Noncommercial 1.0.0

The recompilation framework and the SNES device models (CPU interpreter, PPU,
APU, DMA, cartridge), linked into `UrbanRecomp.exe`.

Required Notice: Copyright (c) 2026 Matthew Stan

<https://polyformproject.org/licenses/noncommercial/1.0.0>. The full text is
`snesrecomp/LICENSE` in the source tree and `licenses/snesrecomp-LICENSE.txt`
in the Windows package. Because of it, the program may be used and passed on
for noncommercial purposes only.

## recomp-ui -- MIT

The launcher. Copyright (c) 2026 Matthew Stanley. Full text:
`recomp-ui/LICENSE`, or `licenses/recomp-ui-LICENSE.txt` in the package.

It bundles:

- **Dear ImGui** -- MIT, Copyright (c) 2014-2025 Omar Cornut
- **stb_image, stb_image_write, stb_truetype** -- MIT or public domain, Sean Barrett
- **tinyfiledialogs** -- zlib, Guillaume Vareille
- **gl_core_3_1** -- generated OpenGL loader

## Fonts and images in `assets/`

- **Lato** (`LatoLatin-*.ttf`) -- SIL Open Font License 1.1, Łukasz Dziedzic
- **Noto Sans Symbols 2** -- SIL Open Font License 1.1, The Noto Project Authors
- **OpenMoji** (`OpenMoji-black-glyf.ttf`) -- CC BY-SA 4.0, OpenMoji Project
- **Country flags** (`flags.png`) -- rendered from Noto Color Emoji, SIL Open
  Font License 1.1, The Noto Project Authors

recomp-ui's own notices for these are in `recomp-ui/assets/common/fonts/NOTICE.md`
and `recomp-ui/assets/common/img/NOTICE.md`.

## SDL2 -- zlib licence

`SDL2.dll`. Copyright (C) 1997-2025 Sam Lantinga.

## Microsoft Visual C++ runtime

`vcruntime140.dll`, `vcruntime140_1.dll` and `msvcp140.dll` in the Windows
package are redistributable files of Microsoft Visual Studio 2022.

## Sylt

`sylt_graphics/sylt_map.bin` and `sylt_card.bin`: the Sylt map is lytron's
work, used with written permission, and the card artwork is this project's
own. See `sylt_graphics/PROVENANCE.md`.

## Credits for ideas, no code taken

- **Truttle1** found the post-load power bug and the power bit.
- **Selicre**'s community mouse patch identified the cursor bytes that the
  mouse control drives (<https://github.com/Selicre/simcity-mouse>).
