# Project name

The project is called **Urban Recomp**. The earlier working name was the
game's own title, a third-party trademark, so it came out of the project's
name, its executables, its files and its prose.

## Done

- [x] Repository, CMake project, targets and executables: `UrbanRecomp*`.
- [x] Source files and identifiers use the project's own `sc`/`Sc` prefix.
- [x] Docs, comments and tools say "the game". Only README.md names it, once,
      together with a trademark notice.
- [x] No ROM file name is assumed anywhere. The launcher lets the player pick
      the file; the host (`ScFindRom`) and the tools (`tools/find_rom.py`)
      recognise a ROM by its contents.
- [x] The logo (`assets/urbanrecomp_logo.png`) is in the README, the launcher
      box art, the window icon and the Windows executable icon
      (`tools/make_brand_assets.py`).

## Left

- Git history and the issue/PR texts already on GitHub still carry the old
  name.

## Not a concern

- The title screen draws the original wordmark and trademark lines. They come
  from the player's own ROM at runtime; the project neither ships nor
  reproduces them.
