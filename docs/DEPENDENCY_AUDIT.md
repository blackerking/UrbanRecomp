# Shared dependency audit

This branch starts from SimCitySNESRecomp upstream `master` at `d54bbbd`
and updates the `snesrecomp` gitlink from `a6a037f` to shared `main` at
`4d42cab`. The engine source is unmodified. `recomp-ui` is added at
`cb7e54b` for the same built-in Mods provider used by Super Metroid.

The old engine URL, `https://github.com/mstan/snesrecomp.git`, is the shared
repository, not a game-owned fork URL. Its pinned history nevertheless contains
six commits outside current shared main:

| Commit | Change |
| --- | --- |
| `0183d9a` | Treat COP as a tier-to-LLE call instead of structural poison |
| `3dbd292` | Add `exit_mx_set` for callees returning in multiple M/X widths |
| `93d4dd1` | Diagnostics for interpreter bridge runs that never return |
| `2a095a1` | Opcode trace markers and an APU diagnostic switch |
| `2c06601` | Deadline reporting at the firing point |
| `a6a037f` | Return a deadline unwind to the host |

This is divergent dependency history despite using the shared repository URL.
The current engine already handles deadline unwinds, but the old COP analysis
and `exit_mx_set` behavior should not be assumed to have landed. In current
Python analysis, BRK/COP still receive structural-poison treatment. The game
configs use `exit_mx_set` in banks 00, 01 and 02; current config parsing does
not implement that directive.

The default game executable is the standalone LLE interpreter host. It does
not compile generated banks or use those directives, so it can run the latest
engine without keeping a private engine patch. This branch adapts its host
to the current shared API: links S-DD1 device support, supplies the device
layer's DMA cycle accumulator, applies charged DMA time through the host's
existing beam loop, holds the shared beam during writes to avoid a second
HDMA/clock owner, and uses the shared SDL3 keyboard adapter.

The experimental AOT targets and `tools/regen.sh` require a separate migration
of the COP/width analysis assumptions before their old qualification claims
can be repeated against this pin. They are not part of this renderer's
validated build. Preserve the six commits as review references; do not remove
the width annotations merely to make regeneration accept the configs.

Reproduce the history check from this checkout:

```sh
git -C snesrecomp remote -v
git -C snesrecomp log --oneline 4d42cab..a6a037f
git -C snesrecomp status --short
git submodule status
```

Build and validation instructions are in [ADAPTIVE_RENDERER.md](ADAPTIVE_RENDERER.md).
