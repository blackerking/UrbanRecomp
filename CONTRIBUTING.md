# Contributing

Thank you for helping improve SimCitySNESRecomp. The project keeps ROM data
and generated game code out of Git, so a working checkout has two explicit
inputs: the pinned `snesrecomp` submodule and your own legally obtained ROM.

## Set up a checkout

```bash
git clone --recurse-submodules <this repo's URL>
cd simcity
bash tools/bootstrap.sh
```

`tools/bootstrap.sh` is safe to rerun. It synchronizes submodule URLs,
initializes nested submodules, and verifies that `snesrecomp` matches the
gitlink committed by this repository.

Stage a verified US ROM as `simcity.sfc` at the repository root and generate
the private C sources:

```bash
bash tools/regen.sh --no-tests
```

The ROM and `src/gen/` are ignored and must never be committed.

## Build and run

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --target SimCitySNESRecomp
build/SimCitySNESRecomp.exe                 # windowed
build/SimCitySNESRecomp.exe --qualify 3600  # headless activity qualification
```

`--qualify N` runs N frames with no window and asserts the same generic bar
snesrecomp's own per-game status table uses for a new bring-up: logic state
must keep changing, audio must stay actively producing samples, and rendered
video must not freeze -- see `src/main.c`'s `run_qualification` and
`snesrecomp/cosim/ref_driver.c` (the framework's own game-neutral reference
driver, which this project's qualification check mirrors).

## Change the framework dependency

The `snesrecomp` gitlink is the single source of truth for the framework
revision. Normal game-only changes should leave it untouched. If you find a
genuine framework bug (not specific to SimCity), fix it upstream in
`snesrecomp` on its own branch, coordinate that separately, and only then
bump this repo's gitlink -- do not carry local patches to the submodule.

## Before opening a pull request

- Rerun `bash tools/bootstrap.sh` and confirm `git submodule status --recursive`
  has no `-`, `+`, or `U` prefix.
- Build the target and run `build/SimCitySNESRecomp.exe --qualify 3600`.
- Run `git status --short` and check for ROMs, generated sources, build
  trees, or unrelated files before staging.
- Keep game-specific addresses and behavior (bank cfgs, `src/variables.h`,
  any future HLE overlays) in this repository. Reusable CPU, mapper,
  coprocessor, diagnostics, and presentation mechanisms belong in
  `snesrecomp` -- see its README's Contributing section.
