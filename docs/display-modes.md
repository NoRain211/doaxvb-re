# Display modes, upscaling and filtering (D3D11)

The launcher's **Display** page sets these; each maps to an environment
variable the runner reads at startup.

| Setting | Variable | Values |
| --- | --- | --- |
| Display mode | `RECOMP_D3D_WINDOW_MODE` | `borderless`, `windowed`, `legacy` (unset) |
| Window size | `RECOMP_D3D_WINDOW_HEIGHT` | client height in pixels; `0` fits 85% of the work area |
| Upscaling | `RECOMP_D3D_UPSCALE` | `fsr` (AMD FSR 1.0) or `bilinear` |
| FSR sharpness | `RECOMP_D3D_SHARPNESS` | `0`..`1`; 1 is RCAS's maximum, 0 two stops softer |
| Anisotropic filtering | `RECOMP_D3D_ANISOTROPY` | `0` (game default), `2`..`16` |

In windowed and borderless modes the swap chain matches the window and the
frame, rendered at the Graphics page's render resolution, is scaled into the
largest rectangle with the game's aspect; the rest is black. The window can be
resized or maximized, and **Alt+Enter** switches between the window and
borderless fullscreen on the window's monitor. The cursor is hidden over a
borderless window. `legacy` keeps the original fixed 640x480 window or
screen-sized popup.

FSR 1.0 (`third_party/fsr1`, MIT) runs EASU into a letterbox-sized texture and
RCAS into the window. It only upscales: at or above the window size the
bilinear pass is used, as it is on hardware below feature level 11_0 or when
the headers are absent at build time.

Forced anisotropic filtering upgrades the samplers of ordinary textured draws
and their combiner stage. Draws that use the game's four-tap filter, an alpha
mask or a vertex/pixel program keep their point-mip clamp sampler, which
those effects depend on, so they are not filtered anisotropically.

DLSS is offered separately, with its own motion vectors and jitter; see
[DLSS](dlss.md).

## Frame rate

The launcher's Display page sets `RECOMP_SPLIT_RATE`. Gameplay, physics and
sound always keep the game's 60 Hz tick; above 60 the render worker presents
each tick several times, interpolating the camera, characters, the ball and
rigid props, one tick behind (see
[the split-rate notes](wayfinder/research/high-frame-rate-split.md)).

| Choice | Value | Presents |
| --- | --- | --- |
| 60 FPS (original) | unset | The game's own 60 Hz path |
| 120 FPS | `120` | 120 per second; even only on a 120 Hz or VRR display |
| Match display refresh | `auto` | The primary display's refresh rate |
| Unlocked | `unlocked` | As fast as the GPU allows (up to 1000), with tearing |

`unlocked` presents through the VRR path (`DXGI_PRESENT_ALLOW_TEARING`, no
vblank wait) whatever the VRR toggle says. Where the GPU or driver does not
support tearing, it still presents without waiting for vblank, and the display
shows the newest finished frame at each refresh; the run log says which.

A tick never takes more presents than fit in it. The worker times a tick's
first present and its later ones separately and spaces presents evenly by how
many fit in 80% of a tick, never faster than the chosen rate; it also stops
early when a present would end past the tick, or when it picked the packet up
more than a quarter tick after the game published it. Without this, a slow
present delayed the next packet, the game waited on the full queue, and play
ran in slow motion (45 ticks per second) instead of at fewer frames.

With sun shadows on, the first replay of a tick draws the sun map and the later
ones reuse it, its projection and caster flags, so shadows move at 60 Hz while
the bodies casting them are interpolated. Each replay still selects the camera
so receivers match the interpolated view.

Observed 2026-11-01, RTX 4060 Laptop and i5-13420H, 144 Hz panel, DLSS
Quality, per-pixel lighting and smooth normals, CPU Exhibition court 2: sun
shadows off, `auto` presents 120 per second and `unlocked` 146 to 184, the game
at 60 ticks either way. With sun shadows on, a present costs about 8.5 ms there
and only one fits a tick, so it stays near 60; plain settings reach 144.
