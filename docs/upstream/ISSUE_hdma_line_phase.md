# HDMA line phase slips on one frame in four (scenario view screen)

**Repo:** mstan/snesrecomp — observed at `251a966` (main) merged into a
downstream fork, i.e. with `b8ef573 runtime: execute HDMA on LLE beam timeline`.

**Game:** SimCity (U), scenario view screen (the isometric city on a desk).

## Symptom

Reported from play as the map "jittering a little bit every couple of frames".

## What it actually is

Frame-by-frame capture at `SC_DUMP_INTERVAL=1` shows a strict period-4 cycle:

    frame 62  ->  63   3671 px change
    frame 63  ->  64   3671 px change
    frame 64  ->  65      0
    frame 65  ->  66      0

and frame 64 is **pixel-identical to frame 62**. So one frame in four renders
differently and the next returns exactly. It is not a moving object.

It is also **not a shift**. Testing frame 62 against 63 over the map region at
every whole-pixel offset, `dx=0, dy=0` is the closest match (3529 differing px)
and every offset is worse:

    dx=-1 dy=0  15050      dx=+1 dy=0  11360
    dx= 0 dy=-1 17986      dx= 0 dy=+1 16718

The structure is the tell. Differing scanlines are:

    43, 47, 51, 55, 59, 63, 67, 71, ... 199, 203, 207

**every 4th line, 42 of them**, evenly spaced across the map, never two
adjacent. A tear would be a contiguous band; a re-render would be dense. A
strict 4-line comb is an HDMA cadence.

`hdmaActive` on this screen is `0xe0` — channels 5, 6 and 7 live on every
frame, which is how the skewed map is drawn.

So: one frame in four, the per-line HDMA writes land on the wrong phase, and
every 4th scanline keeps the previous frame's value.

## Not widescreen

Identical with the widescreen extension disabled (256 px output), so it is not
the margin path.

## What I could not establish

Whether this predates `b8ef573`. I have no A/B, because the save-state format
diverged in the same range: states written by the newer build are refused by
the older one, and states written by the older build are refused by the newer
(`RTL_SAV_VERSION_MIN` raised to 6). A cold boot does not reach this screen
without menu input I could not script reliably.

Worth flagging separately: the OLD runner does not refuse a NEW-format state,
it **silently mis-loads** it — `logic_changes=0`, `nmi_serviced=0`, wedged at
`cb:0d33`. The version gate protects the new build from old files but not the
reverse, which is the same silent-corruption failure the gate was added to
prevent.

## Reproduction

A save state on this screen reproduces it on current `main` (it is in the new
format, so it loads there). Available on request — this repo is ROM-free and
does not carry states, so it is not attached here.

    SC_DUMP_INTERVAL=1 SC_DUMP_START=60 <runner> --load-state <state> --qualify 120

then diff consecutive frames; the period-4 cycle and the every-4th-scanline
comb are immediate.
