# PR draft — `interp_bridge`: hand a deadline unwind back to the host

**Status: not opened.** Branch is ready locally; when it goes up it goes
via a fork under `blackerking`, not straight to `mstan/snesrecomp`.

Branch: `pr-deadline-unwind` (1 commit, off `origin/main`)
Files: `runner/src/snes/interp_bridge.c` only, +32 / -3

## The bug

A yield primitive and a master-deadline expiry both reach the bounce site
through the same `interp_bridge_lle_yield_unwind()` sentinel, and both were
treated identically: consume the request and resume interpreting at the
primitive entry.

That is correct for a yield primitive and wrong for a deadline. The deadline
exists because the **host** asked for a time bound, so the host has to regain
control. Resuming inside the bridge leaves the bound still expired, so the very
next bounce unwinds again, and the host never gets a chance to re-arm.

A `interp_bridge_run_loop()` caller therefore hangs — and there is no step cap
to catch it, because bounced compiled bodies do not count interpreted steps.

## The fix

Record *why* the unwind was raised, in the deadline predicate where that is
known. A scheduler-mode deadline unwind then syncs, flushes the APU, publishes
the resume PC and returns 1 — exactly what the vblank yield path already does.

Yield-primitive unwinds are untouched. The auto-quiescent path is explicitly
excluded (`yield_pc && !auto_quiescent`).

## Evidence

Driving SimCity's guest through `interp_bridge_run_loop`:

```
before: frame 1 enters at 008000 and never returns
after:  frame 1 returns ok=1, frame 2 resumes at 008D65
```

Boot needs ~2.87M master cycles before its first vblank wait, so a one-frame
bound legitimately expires mid-boot. This is what lets a host carry that across
frames instead of deadlocking on it. With the fix the same host runs 600 frames
green: 600 NMIs serviced, `logic_stall_max` 0.

`tests/run_tests.py`: **81 passed, 0 failed.**

## Why it may not have been hit before

It only bites a host that (a) drives the guest through `run_loop` rather than
per-opcode, and (b) arms `interp_bridge_set_master_deadline()`. A game whose
boot reaches its first yield primitive inside one deadline never sees it.
