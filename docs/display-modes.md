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
