Draft reply to mstan/snesrecomp PR #17 ("joypad: cover auto-read register byte
order"). Not posted — see the note at the end.

---

Thanks for picking this up, and for adding a regression test — that's more
useful than the swap I proposed.

I think we're both right, about different hosts, and the real problem is that
the contract for `input*_currentState` isn't written down anywhere.

**Measurement first.** Reverting only the `$4218`/`$4219` hunk of my local fix,
so the runner behaves exactly as this PR does, and holding Right on a fixed
save state for 110 frames (SimCity, cursor X at `$7E01EB`):

```
with my swap        f0:x=128  f40:x=178  f80:x=232  f120:x=232   (moves, then clamps)
with this PR's order f0:x=128  f40:x=128  f80:x=128  f120:x=128   (dead)
```

Same binary otherwise, same save state, same input script. No crash — the
input just never arrives. So for this host, the current order does not work.

**Why I think that's not a contradiction.** Your note says
`input*_currentState` "is already in serial order (B,Y,Select,Start,Up,Down,
Left,Right,A,X,L,R)". This host does not supply that. Its pad constants are

```c
kPad_Right = 0x8000, kPad_Left = 0x4000, kPad_Down = 0x2000, kPad_Up = 0x1000,
kPad_Start = 0x0800, kPad_Select = 0x0400, kPad_Y = 0x0200, kPad_B = 0x0100,
kPad_R = 0x0008, kPad_L = 0x0004, kPad_X = 0x0002, kPad_A = 0x0001,
```

i.e. the reverse of serial order, which is why an extra reversal lands it
correctly and why removing one breaks it. Those constants were derived
empirically against this runner, button by button, before I touched `snes.c` —
so the host and the runner were already disagreeing, and my swap papered over
it on the runner side rather than the host side.

If serial order is the intended contract, then **my fix is wrong and the right
change is in my host**, not in `snes.c`. I'm happy to make that change here and
close #14. What would help is having the contract stated somewhere — a comment
on `input1_currentState` or in `joypad.h` saying which bit is B and which is
Right — because right now a host author has no way to know except by
bisecting buttons against a game, which is exactly what I did and got a
plausible-but-different answer from.

One thing worth checking on your side: SMK passing doesn't distinguish the two
readings unless its host populates `input*_currentState` differently from
mine. If both hosts feed the same convention and only one works, that's a real
divergence; if they feed different conventions, the test is locking in one of
them and the other host needs fixing.

Also relevant: a sibling project (Metal Marines) hit the identical symptom
independently. That's what made me suspect the runner rather than the game — two
unrelated commercial titles don't ship the same input bug. But it equally
supports "two hosts derived from the same wrong assumption", so I don't lean on
it.
