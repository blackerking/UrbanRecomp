# PR draft — cfg: `exit_mx_set` for callees that exit in several widths

**Status: not opened.** Branch is ready locally; when it goes up it goes
via a fork under `blackerking`, not straight to `mstan/snesrecomp`.

Branch: `pr-exit-mx-set` (1 commit, off `origin/main`)
Files: `recompiler-rs/src/cfg.rs`, `recompiler-rs/src/bin/analyze.rs`,
`recompiler/v2/cfg_loader.py`, `tools/v2_regen.py` — +213 / -2

## The gap

Neither existing directive can express a callee that is entered at one width and
returns at more than one:

| directive | says |
|---|---|
| `exit_mx_at <addr> <m> <x>` | one exit width, broadcast to every entry variant |
| `exit_mx_at_per_variant` | one exit width per entry variant |

A routine that is entered at `M0X0` and returns in **either** `M0X0` or `M0X1`
depending on the path has no representation. Publishing one of the two is worse
than publishing neither: the decoder forks the post-call continuation once per
declared width, so an omitted width is a continuation that never gets decoded.

## The directive

```
exit_mx_set <addr> <entry MmXn> <exit MmXn>[,<exit MmXn>...]
```

Real cases from SimCity, all measured against an executed-PC bitmap split by the
live (m,x) flags:

```
exit_mx_set 028000 M0X0 M0X1,M1X1
exit_mx_set 00c3f9 M0X0 M0X0,M0X1
exit_mx_set 01ac23 M0X0 M0X0,M0X1
```

Three tests included. Parsed by both the Rust analyzer and the Python
`cfg_loader`, so the two front ends stay in step.

## Why this matters for coverage

An unpublished or under-published exit truncates **every** caller transitively.
On this game, closing the exit-M/X gap moved AOT-eligible coverage from 96.5% to
99.3% of executed code (1,475 -> 1,544 variants), and `exit_mx_set` covers the
cases the other two directives structurally cannot.

`tests/run_tests.py`: **81 passed, 0 failed.**
