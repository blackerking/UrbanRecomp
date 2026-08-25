# A master-deadline expiry in scheduler mode never returns to the host

**FILED: https://github.com/mstan/snesrecomp/issues/23 — FIX IN PROGRESS UPSTREAM.**

`origin/codex/lle-deadline-unwind` (`4454da6`, "runtime: return on scheduler
deadline unwind") implements the same shape as our patch: latch that the unwind
came from a deadline where the deadline is detected, then branch on it in the
unwind handler to publish the resume PC and return to the host.

Theirs is better than ours in two ways — it also clears `s_lle_unwind_active`
and `s_lle_unwind_owner_depth` on that path, which our version left set, and it
adds a regression test in `tests/interp816/bridge_test.c`.

**NOT merged into `main` yet**, so `main` still has the bug. Our local
`pr-deadline-unwind` branch is superseded and should be dropped rather than
offered, once theirs lands.

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

## Rebase trial, 2026-08-25

Rehearsed on a scratch branch in the submodule (`scratch-rebase`, with
`scratch-rebase-backup` pinning the old tip). Base chosen as
`origin/codex/lle-deadline-unwind` rather than `origin/main`, so the trial gets
the new main *and* the deadline fix in one go.

Of our nine local commits:

| commit | result |
|---|---|
| Model COP as a tier-to-LLE call | clean |
| `cfg: exit_mx_set` | clean |
| deadline unwind | **dropped** — upstream's `4454da6` supersedes it |
| bounce/step counters | 1 trivial conflict, both sides kept |
| host coverage hooks | 1 conflict; kept the hook, dropped a `pctrace` reference belonging to a commit not carried over |
| two diagnostics for runs that never return | not carried |
| pctrace markers + APU bisect switch | not carried |
| deadline-fired reporting | not carried |
| `SNESRECOMP_REGWRITE_DIAG` | not carried |

The four left behind are pure diagnostics with no external API. The two that
were carried are load-bearing: `main.c` calls `interp_bridge_bounces`,
`_steps`, `_bounce_hook` and `_pc_hook`, none of which exist upstream.

Result: **builds clean and behaves identically.**

```
default  PASS frames=600 master=214385616 logic_changes=592 video_changes=174
SC_FIBER PASS frames=600 master=214385616 logic_changes=592 video_changes=174
                 nmi_requests=592 nmi_serviced=592
```

`SC_FIBER` matters most here — it is the path the deadline fix exists for, and
upstream's version carries this host as well as ours did.

The submodule was then restored to `bedf078` and rebuilt, so the working tree is
unchanged. The pin is still NOT moved: `codex/lle-deadline-unwind` is not merged
into `main`, and four of the commits on the scratch branch exist only locally,
so committing that pointer would reference hashes nobody else can fetch. Move it
once that branch lands.
