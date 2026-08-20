# A new renderer: what it would mean, and what is already built

Prompted by the map-scroll limitation. Two very different things get called
"a new renderer", and only one of them turns this project into a PC port.

## The two meanings

**Presentation.** Take the guest's 256x224 frame and display it better --
aspect, shaders, upscaling, wider viewport. The guest still draws everything.

**Semantic.** Draw the map ourselves from game state, at any scroll rate,
resolution or zoom. The guest's map-drawing code stops mattering.

Only the second removes the scroll limitation, and only the second costs the
oracle.

## What already exists (do not rebuild these)

| piece | where | what it gives |
|---|---|---|
| Aspect/viewport | `runner/src/desktop/display_aspect.h` | 4:3, 8:7, 1:1; explicitly presentation-only |
| Shaders | `runner/src/desktop/glsl_shader.c/.h` | GLSL post-processing |
| Frame-model host | `runner/src/desktop/mmx23_host_main.inc` | the reference host loop |
| **Per-layer widescreen** | `runner/src/snes/ppu.h` | `wsLayerWidenMask`, plus per-layer **clamp / mirror / repeat / stretch / anchor**, each with scanline bands |
| **HUD reservation** | same | `wsHudOamFirstSlot/Slots/Height` -- keeps a HUD out of the widened area |
| **Per-line host callback** | `ppu->widescreenLineEnhancer` | the host participates per scanline |
| **Layer capture** | `wsMode2CaptureLayer`, `wsMode2Capture[224][x]` | palette indices of one captured layer, per frame |

This is the important finding. The runner is not a plain PPU with a framebuffer
out the back: it already has hooks for a host to observe and extend individual
BG layers. A SimCity map renderer would plug into that architecture rather than
be built outside it.

ar-recomp's reusable contribution is the **presentation** half (shaders,
aspect, and the settings-overlay pattern this project already borrowed for the
F10 menu). It has no map renderer to lift; that part is ours either way.

## What is actually at stake

Every verification method this project has -- `--qualify`, WRAM differentials,
framebuffer comparison between tiers, the exit-M/X cross-checks, the whole
"does the interpreter do this too?" question -- rests on **the guest code being
the thing that runs**. That question is how the UFO, the meltdown, the three
SDL3 defects and the fiber divergence were each settled.

Replace a subsystem and that subsystem has no reference implementation. So the
rule worth holding: **the guest stays authoritative for STATE; the host may
take over PRESENTATION.** Draw the map differently, but never compute it.

## Staged plan, cheapest first

### Stage 0 -- presentation only. No oracle loss.

Adopt `display_aspect.h` and `glsl_shader`. No risk, and it is the part
ar-recomp genuinely hands us.

### Stage 1 -- widescreen. Still the guest's renderer.

Turn on the existing per-layer widescreen so more map is visible at once.
Supported runner infrastructure, not a hack, and it attacks the original
complaint from a different angle: if more map fits on screen, less scrolling is
needed. The HUD reservation fields exist precisely so the status bar does not
tile into the margins.

**Cheap to try, and worth trying before anything below.**

### Stage 2 -- host-drawn map layer, composited.

Use `wsMode2CaptureLayer` and the line enhancer to substitute a host-rendered
map for the guest's map layer, keeping the guest's UI layers and sprites on top.

The inputs are already ours: the live 120x100 map sits at `$7F0200` and is
fully decoded (`docs/REFERENCE_map_format.md`, `tools/extract_maps.py`), and
tile graphics come out via `tools/extract_graphics.py`.

Keep it opt-in with the guest path intact, exactly as SDL2 was kept beside
SDL3 -- then the two can be compared frame by frame, which preserves an oracle
even for the replaced subsystem.

### Stage 3 -- free scrolling and zoom.

Once the map is host-drawn, scroll rate and zoom become host concerns and the
`$01bd`/`$01bf` limitation is gone. The guest is still told where the camera
is, so its own logic stays consistent.

## Recommendation

Do Stage 1 first. Hours rather than weeks, uses supported infrastructure, keeps
every existing check valid, and may well be enough -- the complaint was
"panning is slow", and showing twice as much map is a different answer to the
same problem.

If Stage 1 is not enough, Stage 2 is well-founded rather than speculative,
because the map format and the layer hooks are both already in hand.

## What would make it a port

Not the renderer. The line is **simulation**: the moment host code decides what
the city does, the ROM becomes documentation rather than the program. Stages
0-3 all keep the guest computing the city.

## Stage 2 progress: layer roles, measured

`SNESRECOMP_LAYER_MASK` (bit0=BG1 .. bit3=BG4, bit4=OBJ) isolates layers as a
host-only render filter -- it never touches guest state. Measured on
`savestate_5`, a live Las Vegas game:

| layer | coverage | top band | bottom band | role |
|---|---|---|---|---|
| BG1 | 2% | 3% | 2% | ~unused |
| **BG2** | **95%** | 94% | 97% | **the map** |
| **BG3** | 26% | **68%** | 9% | **HUD / status bar** |
| BG4 | 0% | 0% | 0% | unused |
| **OBJ** | 12% | 18% | **0%** | **sprites / cursor** |

So the composite is: host-rendered map, plus the guest rendered with
`SNESRECOMP_LAYER_MASK=0x14` (BG3 | OBJ) laid over it. That keeps the HUD, the
status bar and the cursor authored by the game -- only the map is ours.

First composite lands 14,774 HUD/sprite pixels, 26% of the frame.

### Known limitation of this first pass

Transparency is inferred from "pixel is not black", because the masked render
writes 0 for transparent and a black HUD pixel is indistinguishable from an
absent one. Good enough to prove the composite, wrong in principle.

The runner already has the correct mechanism: `PpuBindOverlaySurface` /
`PpuOverlaySource` -- "renderer-neutral host-overlay extraction", with its own
`docs/HOST_OVERLAY_EXTRACTION.md`. Switch to that before this goes near the
host, rather than shipping a black-key hack.

## The right compositing mechanism, and it is the ActRaiser one

`snesrecomp/docs/HOST_OVERLAY_EXTRACTION.md`: the PPU can export selected,
already-rendered layers into **transparent ARGB surfaces** without touching
VRAM, OAM, WRAM, registers, DMA or savestate data. It was *"ported (additively,
preserving this engine's existing widescreen layer-policy API) from Derrick
Gold's ActRaiser fork"* -- so the ActRaiser reuse asked about is real, and it is
exactly this.

```c
PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg3, bg3_argb, pitch);
PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj, obj_argb, pitch);

/* once per emulated frame, before scanout */
PpuClearOverlayCaptures(ppu);
PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg3, 0, 0, 256, 224,
                     kPpuOverlayFlag_RemoveFromGame);
PpuSetOverlayCapture(ppu, kPpuOverlaySource_Obj, 0, 0, 256, 224,
                     kPpuOverlayFlag_RemoveFromGame);
```

Two properties make this the correct basis rather than the black-key hack:

- **Real alpha.** A black HUD pixel and an absent one stop being the same
  thing, which the current composite cannot distinguish.
- **Provably inert when unused.** "With no surface bound and no capture
  rectangle configured, every source is a deterministic no-op ... authentic/
  headless/oracle output is byte-identical." That is precisely the property
  this project needs: the oracle survives the feature existing.

The boundary it defines also matches the state/presentation line already drawn
here -- the runner isolates and decodes layers, the *game policy* decides which
rectangle and when, and the host frontend composes. Only the last two are ours.

### Remaining work to make Stage 2 real

1. Port `tools/render_map.py` into the host in C (the larger piece).
2. Bind BG3/OBJ overlay surfaces and composite with real alpha instead of the
   black key.
3. Wire the map render through `PpuSetWidescreenLineEnhancer` so the widescreen
   margins get genuine map instead of the tilemap repeat.

Steps 2 and 3 are mechanical once 1 exists. Step 1 is where the effort is.

## Stage 3 reached: widescreen margins now carry real map

The original complaint about widescreen was that the map repeated left and
right. That was the guest BG tilemap wrapping past its 32/64-tile width. The
host renderer reads map cells by absolute position and does not wrap, so
`SC_WIDESCREEN` and `SC_HOST_MAP` together do what widescreen alone could not.

Measured at `SC_WIDESCREEN=96` (448x224): **0 of 96** left-margin columns are
identical to a column 256px to their right. Independent map, not a repeat.

### Seeing more map: what each control does

| control | effect | ceiling |
|---|---|---|
| `SC_WIDESCREEN=<px/side>` | wider picture, more map columns | **96/side** (`kPpuExtraLeftRight`), i.e. 448x224 |
| `+` / `-` | pixels per map cell, 2..32 | zoom out shows far more map in the same frame |
| `--scale <n>` | window magnification only | no extra map |

Zoom is the cheap way to see more map, and it has no PPU-imposed ceiling: at 4
px per cell a 256-wide frame covers 64 map cells instead of 32.

### Why the window cannot simply grow further

The framebuffer is the PPU render target, and the PPU caps the widescreen
border at `kPpuExtraLeftRight = 96` per side. Height has no equivalent knob at
all -- 224 lines is the render.

Going beyond that means decoupling the host map canvas from the PPU buffer:
draw the map into a surface of arbitrary size and composite the guest 256x224
output onto a region of it. That is a real change rather than a knob, and it
raises a question worth answering first -- the HUD comes from the guest at a
fixed 256x224, so on a much larger canvas it either floats in the middle,
stretches, or has to be repositioned. None of those is obviously right, and it
is a design decision rather than a bug.

## Where the host renderer stands

Working, opt-in via `SC_HOST_MAP=1`:

- Map drawn entirely host-side from `$7F0200` + VRAM CHR, at 60fps
- Master brightness applied, so fades match the guest
- HUD, sprites and **any** menu composited back from a second render pass that
  captures every layer except BG2 -- self-correcting, no screen-mode flag needed
- Zoom on `+`/`-`, 2..32 px per cell (8 native)
- Widescreen margins carry real map, not a tilemap repeat
- Non-map screens pillarbox instead of tiling

### Known gaps

**The cursor at non-native zoom.** Sprites come from the guest at a fixed 8 px
per cell while the map scales, so anything other than zoom 8 has the cursor
pointing at the wrong cell. Native zoom is exact.

**The overlay export is unused.** `PpuBindOverlaySurface` /
`PpuSetOverlayCapture` arm cleanly, report success, and export zero pixels;
every condition on this side checked out (mode 1, hooks reached, bindings
surviving reset, clear-before-draw order, matching y convention). The
layer-mask two-pass route replaced it. Worth reporting upstream if someone
confirms it independently -- it may be a real bug in the export path.

**Region-locked.** The tile tables at `02:d6a9` are US addresses, so the
renderer refuses on E/F/G/J. Those images run fine on the stock path.

### Cost

One extra PPU pass per visible line while enabled. The guest still computes
everything; only the map is drawn differently, so every existing check remains
valid and the default build is byte-identical to baseline.

### There is no per-screen work

Worth stating, because it is the thing that decides whether this design scales.
Every save state, at `SC_WIDESCREEN=96` with the host map on:

```
$01df  path        states                    margin repeats
  3    host map    0, 2, 3, 4, 5, 6, 7, 8         0/96
  1,4  pillarbox   9, 1                           0/96
```

Two paths, neither of which knows which screen it is on:

- The map path keys on "capture every layer except BG2", so any menu drawn on
  any other layer wins automatically. That is why the in-view menu works
  without being special-cased -- it never changes `$01df`.
- The pillarbox path keys on "the host map is not drawing", so it covers every
  non-map screen that exists, including ones nobody has looked at.

The clean-start bug was not a screen-specific fault; it was a missing generic
path, and adding it fixed the whole class at once.

This is the concrete difference from going fully host-side. Reimplementing the
screens would be per-screen work without end -- every menu tilemap, font,
dialog and animation -- and with no oracle, since the guest would no longer be
drawing the thing being checked.

