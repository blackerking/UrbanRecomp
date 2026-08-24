# A master-deadline expiry in scheduler mode never returns to the host

**Status: NOT FILED.** Draft only — posting is the user's call.

Checked against `origin/main` @ `fe6045c` (post-DKC2 rewrite of
`interp_bridge.c`). The bug survives that rewrite.

## What happens

`interp_bridge_lle_yield_unwind()` is the single sentinel for two very
different requests:

* a **yield primitive** (vblank wait, task switch) wants the interpreter to
  resume at the primitive's ROM entry and carry on — the historic behaviour;
* a **master-deadline expiry** wants the opposite. The host asked for a time
  bound, so control has to leave the bridge entirely.

In scheduler mode (`yield_pc != 0`) the unwind handler treats both the same
way. In current `main` that is `runner/src/snes/interp_bridge.c` around the
`s_lle_unwind_active` block:

```c
s_lle_unwind_active = 0;
s_lle_unwind_owner_depth = 0;
sync_cpu_to_interp(cpu, &in);
in.k  = (uint8)((s_lle_unwind_pc24 >> 16) & 0xFF);
in.pc = (uint16)(s_lle_unwind_pc24 & 0xFFFF);
...
continue;                 /* <- resumes interpreting, deadline still expired */
```

There is no test for *why* the unwind was raised. The deadline is only
consulted on the `auto_quiescent` path:

```c
if (auto_quiescent && s_lle_master_deadline &&
    cpu->master_cycles >= s_lle_master_deadline) {
```

so a scheduler-mode host that sets a deadline gets the unwind, resumes
interpreting with the bound *still* expired, unwinds again on the next bounce,
and never regains control. The host cannot re-arm, because it never runs.

## Why it is easy to miss

Nothing errors. The bridge keeps working, the game keeps running, and the only
symptom is that the host's time bound does nothing — which looks like the host
asked for the wrong bound rather than like a bridge bug. It only bites hosts
that both drive `interp_bridge_run_loop()` themselves and use
`interp_bridge_set_master_deadline()`, which is why `RtlRunFrame` never sees
it.

## Shape of the fix

Record the cause where it is known — at the point the deadline is detected —
and branch on it in the unwind handler: return to the host and publish the
primitive entry as the resume point, so the next call continues exactly where
this one stopped.

We have this as a patch (~+32/-3, `interp_bridge.c` only) against the previous
`main`; happy to rebase onto `fe6045c` and send it if that is welcome.
