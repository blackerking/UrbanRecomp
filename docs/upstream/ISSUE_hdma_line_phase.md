# HDMA line phase slips on one frame in four (scenario view screen)

**Repo:** mstan/snesrecomp — observed at `251a966` (main) merged into a
downstream fork, i.e. with `b8ef573 runtime: execute HDMA on LLE beam timeline`.

**Game:** the city builder this host targets (US), scenario view screen (the isometric city on a desk).

## Resolved (2026-09-17): the host lost the beam, not the HDMA engine

Root cause found with a save state in the current format. In the jittering
frame every HDMA write of all three channels -- including channel 5, whose
table is in ROM and cannot change -- lands exactly **one line late** (L44
instead of L43, L8 instead of L7), and the next frame is back on time. So a
line's HDMA step was skipped, and everything after it slid down a line.

It was skipped because snes.c moves the beam past the host's own driver
(`handle_pos_stuff()` in `src/main.c`), in two places:

1. **`$4212` reads.** Each read adds a synthetic 64-clock step unless
   `g_interp_apu_driving` is set. This host advances the beam from every
   opcode already, but left the flag at 0. A step that jumps over h=1024
   loses that line's HDMA transfer.
2. **DMA starts (`$420B`).** The transfer's guest time is charged through
   `snes_set_master_clock_charge_hook()`, and without a hook snes.c moves
   hPos/vPos itself. The host never draws the lines crossed, never runs
   their HDMA step, never counts a crossing of the frame end, and never gives
   the APU that time.

Fix: `sc_own_the_beam()` installs a charge hook that walks the time through
`handle_pos_stuff()` (and the APU), and sets `g_interp_apu_driving`.
`SC_BEAM_LEGACY=1` restores the old behaviour for A/B runs.

Measured on the report's screen, 900 frames: 58 one-frame outliers before, 0
after. The second path also explains crackling audio: in a city
(`savestate_2`) the APU got about 94% of its time, 503.5 samples per frame
against the 533.1 the output drains, so the queue kept running dry. After the
fix: 533.7. The attract demo still passes, and so do all ten local states;
its frames shift slightly in time because DMA time now counts.

### Follow-up: sound behind the picture

Once the DMA time reached the APU, the sound drifted behind the picture. Two
causes, both fixed in `src/main.c`:

- `kApuCyclesPerMaster` used LakeSnes's `/ (1364*262*60)`: 60 fps instead of
  60.0988, which is 534 samples a frame against the 533.12 the output
  plays. The surplus had been hidden by the missing DMA time. The constant is
  now `32040*32 / 21477272` (533.125 a frame), and the drain takes exactly
  that (`kDspSamplesPerFrame`).
- The audio device keeps its own clock. Over RDP ("Remote Audio") it measured
  0.76% to 1.4% slow, varying between sessions, so the device queue grew
  without bound. `sc_audio_rate_control()` now resamples each frame by a
  small ratio steered by the queue level (proportional plus a learned drift,
  tuned in a simulation). Measured over 75 s: queue 2400-4400 samples, learned
  drift -1.2%, no drops, the DSP backlog at 0-1 sample. Past fast-forward the
  DSP backlog is trimmed to one frame.

`SC_AUDIO_DEBUG=1` prints the queue, the DSP backlog and the learned drift
every 180 frames.

Upstream angle, if it is worth raising: a host that drives the beam itself has
to know about both paths, and nothing in `snes.h` says so.

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
