# `SNESRECOMP_YIELD_STACK_DIAG` is gated on a hardcoded frame number

**Status: NOT FILED.** Draft only — posting is the user's call.

`origin/main` @ `fe6045c`, `runner/src/snes/interp_bridge.c`:

```c
if (getenv("SNESRECOMP_YIELD_STACK_DIAG") &&
    snes_frame_counter >= 5390) {
```

The `5390` looks like a leftover from bisecting one specific title: setting the
documented environment variable produces no output at all until frame 5390, and
on a run that never reaches that frame it produces none ever. Someone turning
the diagnostic on to investigate an early-boot yield problem sees silence and
reasonably concludes the switch is broken.

Suggested: drop the frame test, or give it its own variable
(`SNESRECOMP_YIELD_STACK_DIAG_FROM=<frame>`) defaulting to 0, so the
environment variable alone is sufficient to enable it.

Trivial either way — filing because it is the kind of thing that costs someone
else an afternoon.
