# Regional ROMs: what is shared, what is not

Five 512KB images: U, E, F, G, J. `simcity.sfc` is byte-identical to the US one.

| region | header byte | FNV-1a32 |
|---|---|---|
| Japan | `00` | `ccb8c347` |
| USA | `01` | `ec01686a` |
| Europe | `02` | `b76b1a0d` |
| France | `06` | `e1f99069` |
| Germany | `09` | `aeca7623` |

## They are two code bases, not one with swapped text

```
          COP     NMI
U, J    $8211   $80b2      identical bytes at 00:8211, 00:80b2, 03:b8ae
E, F, G $820d   $80ae      consistently 4 bytes earlier
```

Only 26% of 1KB chunks are byte-identical to U, for both J and E, and the best
whole-ROM shift against U is **+0** -- so E/F/G are a different build rather than
a relocation of the US one.

What *is* shared is data: banks `04`-`08` are largely identical across regions
(36KB runs in `04:8000`-`05:8fff`, and `06:fc00`-`08:b3ff` for J). Banks `00`-`03`
are the code and they genuinely differ. J is closest to U and is the sensible
first target for any per-region recompilation.

## The interpreter runs every region unchanged

All five pass `--qualify 600` with essentially identical counters (592 logic
changes, 174 video changes; G differs by one frame of audio). The interpreter
tier is ROM-agnostic, so `SC_LANG=U|E|F|G|J` is all a language selector needs.

## The AOT tier is US-only, and is now guarded

Generated code carries US addresses and a US dispatch table. `SC_FIBER` on any
other image is refused outright.

> The first version of that guard sat where `SC_FIBER` is parsed and did nothing
> at all, because env parsing runs **before** the ROM is read -- a German ROM ran
> 60 compiled bounces straight past it. It now checks where the fingerprint is
> actually known.

The default AOT path is safe regardless: it reports `bounces=0`, i.e. it is pure
interpreter unless `SC_FIBER` is set.

## US byte-patches must be fingerprint-gated

The cursor-cadence patch tests a **single byte** (`== 0x03`), which cannot
identify a site in a different build -- measured, it was patching **2/2 sites on
every one of E/F/G/J**. Now gated on the US fingerprint, so it reports `0/2`
there.

The view fix tests four bytes (`8f b5 21 7e`, a `STA $7e21b5` long) and already
rejected foreign ROMs on its own ("byte mismatch"). Gated too, for consistency.

The lesson generalises: a byte-signature patch is only as specific as its
signature, and one byte is not a signature.
