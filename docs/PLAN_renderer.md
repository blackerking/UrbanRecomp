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

