# AOT and interpreter disagree on the abs,X / abs,Y read page-cross penalty

*Written by an AI (Claude) working on a SimCity SNES recomp host with
@blackerking, who has reviewed it. Every number below is from a run of this
repo's own `SimCityAOTDiff` differential tool.*

## What happens

`SimCityAOTDiff` reports **cycle counts agreeing on 51 of 64 trials**. Results
themselves are fine — `divergent: 0`, so WRAM and A/X/Y come out identical.
Only the accounting differs, and the AOT tier is always HIGH, always by a
multiple of 8 master clocks (one CPU cycle in an 8-clock region):

```
00:8982  AOT 760 vs interp 744  (+16)   on 4 of 8 randomised trials
00:8924  AOT 760 vs interp 744  (+16)   on 1 of 8
00:D23A  AOT 240 vs interp 232  (+8)    on 1 of 8
```

## The cause

The delta is exactly 8 master clocks per `abs,X` **read** in the body, on the
trials where the randomised index happens to cross a page:

| body | abs,X reads | delta |
|---|---|---|
| `00:D23A` | `LDA $d193,X` | +8 |
| `00:8924` | `LDA $896a,X`, `LDA $896c,X` | +16 |
| `00:8982` | `LDA $89c8,X`, `LDA $89ca,X` | +16 |

`00:D23A` is the whole body, and there is nothing else in it:

```
REP #$30 / LDA $0421 / ASL A / TAX / LDA $d193,X / STA $7e2000 / RTS
```

Its generated `M0X0` variant contains exactly two cycle charges — the static
block cost, which matches the interpreter exactly, and one conditional:

```c
cpu->cycles += 29;
cpu->master_cycles += 232;                    /* == interp's 232 */
...
if ((0xD193 & 0xFF00) != ((0xD193 + cpu->X) & 0xFF00)) {
    cpu->cycles += 1; cpu->master_cycles += 8;
}  /* abs,X read page-cross */
```

Emitted by `recompiler/v2/emit_function.py`, `_runtime_charges()`, under
`'xcross'`.

The interpreter does not charge this. `interp816_adrIdy()` and its siblings
gate the penalty on the opcode **writing**:

```c
if (write && (!cpu->xf || ((pointer >> 8) != ((pointer + cpu->y) >> 8))))
    cpu->cyclesUsed++;
// x = 0 or page crossed, with writing opcode: 1 extra cycle
```

`LDA abs,X` is a read, so the interpreter charges nothing at all.

## Why this looks like a real bug rather than a tie

The emitted test does not consider the index width, even though the recompiler
emits a separate function variant per M/X combination and therefore knows it
statically. The body above is the **`M0X0`** variant — 16-bit index. The
page-cross penalty on the 65816 exists because an 8-bit index lets the CPU
speculate on the low byte and re-issue when the carry propagates; with a 16-bit
index there is no speculation to correct, so at `x=0` there should be no
penalty to charge.

So on `M?X0` variants the charge appears to be unconditionally wrong, and every
one of the 13 disagreements above is on an `X0` body.

I have deliberately **not** changed this. The docstring says the model was
"measured against bsnes", and bsnes charges an idle cycle on
`!x || page-crossed` for indexed reads — i.e. *always* when `x=0`, which is a
third answer, and the opposite direction from the AOT's. Three plausible rules
are in play and I cannot tell from here which one this project intends:

1. never for reads (what the interpreter does),
2. on page-cross only (what the AOT does),
3. always when `x=0`, else on page-cross (bsnes).

Whichever is right, **the two tiers should agree**, and today they do not.

## Reproduction

```
cmake --build build --config Release --target SimCityAOTDiff
./build/Release/SimCityAOTDiff.exe
```

Look for `cycle counts agree: 51 of 64 trials` and the `cycles: AOT ... vs
interp ...` lines above it. The trial-dependence is the tell: the same body
agrees on most randomised trials and disagrees on the rest, because the
randomised `X` decides whether the page is crossed.

Note the tool did not build in this checkout until just now — it linked
`simcity_fiberdrive.c`, which it never uses, and failed on
`sc_advance_until_input_ready`. If `SimCityAOTDiff` has been unbuildable
upstream too, this disagreement would not have been visible.

## Ruled out along the way

- **A per-`STA long` overcharge.** Correlates perfectly across all three bodies
  (one long store → +8, two → +16), but the static block cost is already exactly
  the interpreter's, so it was a coincidence of these three bodies.
- **The AOT prologue's stack reads.** The generated function recovers a host
  return PC with `cpu_read8()` depending on `host_return_valid`, which varies
  per trial and fit the symptom well. But `cpu_read8()` returns early for WRAM
  through `cpu_wram_offset()` and charges nothing; only hardware registers reach
  `cpu_pace_cycles()`, and the stack is WRAM.
