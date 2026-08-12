Draft reply to mstan/snesrecomp PR #17. Supersedes the earlier draft, which
argued the runner was wrong. It isn't.

---

You're right, and #14 should be closed as invalid — the bug was in my host, not
in `snes.c`.

`input*_currentState` being serial order, LSB first, is the piece I had wrong.
My host was filling it in the reverse order and I "fixed" the runner to
compensate, which is why my swap looked correct against one game.

Verified with your byte order and the submodule **unmodified**, holding each
direction for 110 frames on a fixed save state:

| held | cursor |
|---|---|
| Right | `$01EB` 128 → 232 (clamps) |
| Left | 128 → 16 |
| Down | `$01ED` 128 → 192 |
| Up | 128 → 24 |

Identical to what my patched runner produced, so nothing in `snes.c` needed to
change. My host now uses

```c
kPad_B = 0x0001, kPad_Y = 0x0002, kPad_Select = 0x0004, kPad_Start = 0x0008,
kPad_Up = 0x0010, kPad_Down = 0x0020, kPad_Left = 0x0040, kPad_Right = 0x0080,
kPad_A = 0x0100, kPad_X = 0x0200, kPad_L = 0x0400, kPad_R = 0x0800,
```

and my `b48daf4` is reverted.

The one thing I'd still suggest: state the convention somewhere near
`input1_currentState` or in `joypad.h`. I recovered it by bisecting buttons
against a game and got a self-consistent but wrong answer, and a sibling
project (Metal Marines) independently did the same. Your new
`auto_joypad_test.c` pins the runner side; a sentence naming which bit is B
would stop the next host author repeating this.

Sorry for the noise on #14 — the report's hardware layout was right but the
conclusion was not.
