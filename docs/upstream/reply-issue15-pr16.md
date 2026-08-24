# Reply draft — issue #15 / PR #16 test result

**Status: NOT POSTED.** Draft only.

---

Tested on SimCity (US), with `pull/16/head` merged onto the runner branch this
host builds against.

**Result: no change for this host.** The rendered frame is byte-identical to
the pre-PR runner — 0 of 57344 pixels differ.

**Why, I think.** The PR drives HDMA from the beam advance in `snes.c`:

```c
if (check_irq && v < 225u && h == 1024u)
  dma_doHdma(snes->dma);
...
if (v >= 262u) { v = 0; if (check_irq) dma_initHdma(snes->dma); }
```

This host advances the beam itself — it owns `hPos`/`vPos` and steps the
devices around `interp_bridge_run_loop()` rather than calling `RtlRunFrame` —
so neither call is ever reached. It is the same shape as trap 1 in #22, "the
beam has two owners".

**Method, since SimCity's HDMA is subtle.** It is observable on exactly one
scanline (row 198) of one screen. Reference measurement, this host's own
HDMA on vs off: 208 pixels differ on that row. Then:

| | vs |  |
|---|---|---|
| PR #16 on, ours off | pre-PR runner, ours off | **0 px** — the PR is not firing |
| PR #16 on, ours off | pre-PR runner, ours on | 208 px on row 198 — matches the HDMA-*off* case |
| PR #16 on, ours on | pre-PR runner, ours on | 0 px — no double transfer, again because the PR's path is not reached |

**This is not a criticism of the fix.** For `RtlRunFrame` hosts it looks right
and I am not in a position to judge it there; it simply does not reach hosts in
our shape. If it would help, the smallest thing that would close that gap is
making the two entry points callable from outside — `dma_doHdma` and
`dma_initHdma` already exist — plus a line in `docs/LLE_SCHEDULER.md` saying a
beam-owning host must call them at `h == 1024` and on frame wrap. That is what
this host ended up doing by hand, and it works.

Happy to re-test any revision.
