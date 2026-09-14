# Adaptive Widescreen screenshots

Captured from the Windows SDL3 build of implementation commit `67e98bd`.
Gameplay images are direct captures of the game window's client area. The
Mods image comes from the shared launcher's screenshot hook. ROMs and local
emulator snapshots are not included.

**Adaptive Fit, wide window:** 1440x405 window, 684x224 logical canvas.
The entire original view stays centered while additional terrain fills the sides.

![Adaptive Fit in a 32:9 window](adaptive-wide.png)

**Adaptive Fit, portrait window:** the same running instance resized to
600x900, with a 256x448 logical canvas. The original view stays visible while
more of the map appears above and below it.

<img src="adaptive-portrait.png" alt="Adaptive Fit after resizing the same instance to portrait" width="300">

**Fixed 21:9:** San Francisco after normal camera panning, in a 1260x540
window with a 448x224 logical canvas. Terrain and buildings extend past the
native view; game-emitted controls and actors retain their original bounds.

![Fixed 21:9 with a populated San Francisco city](fixed-21x9.png)

**Shared recomp-ui Mods controls:** the built-in feature enabled with 21:9
selected and the original view centered.

![Adaptive Widescreen settings in the shared Mods launcher](mods.png)

These screenshots show the current draft. Dialog centering independent of a
top-left gameplay/HUD anchor remains a follow-up; see
[renderer scope and validation](../../ADAPTIVE_RENDERER.md).
