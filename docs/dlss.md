# NVIDIA DLSS Super Resolution (D3D11, local builds only)

`RECOMP_D3D_DLSS=dlaa|quality|balanced|performance|ultra` (the launcher's
Display page) renders the 3D scene at DLSS's optimal input size for that mode
and reconstructs it at the render resolution set on the Graphics page; the HUD
then draws at that full resolution. DLAA keeps the input at full size and uses
DLSS only for anti-aliasing. Window scaling (bilinear or FSR 1.0) still runs
after it when the window differs from the render resolution.

## License

The NVIDIA RTX SDKs license does not allow the SDK to become subject to an
open-source license. The port is GPL, so DLSS is not part of it: this
repository holds only `recomp-runtime/dlss_bridge.h`, a C interface with no
NVIDIA code. The runner loads `dlss_bridge.dll` at run time only when DLSS is
chosen. The bridge DLL, its source, the SDK and `nvngx_dlss.dll` are built and
kept locally and are never published with the port; `.gitignore` lists them.

## Building the bridge

Write a DLL exporting the functions declared in `dlss_bridge.h` on top of the
NVIDIA DLSS SDK (github.com/NVIDIA/DLSS), built with MSVC because NVIDIA ships
MSVC libraries, and place it with `nvngx_dlss.dll` beside the runner. Without
them, or on a GPU without DLSS, the log says why and the game renders as
before.

## How a frame flows

1. Frames start on the scene target set: colour and readable depth at the
   DLSS input size. `mainWidth()`/`mainHeight()` report that size, so scene
   draws, target scaling and back-buffer copies follow it.
2. Scene draws take the jitter (8 x upscale ratio squared Halton phases, at
   least 8) and name the camera; see [motion vectors](motion-vectors.md).
3. At the first screen-space draw on the main target, or at present, the
   motion pass runs, DLSS evaluates into an output-sized RGBA8 texture, and
   the presenter switches to the full-size set, copies the result into its
   colour, and clears its depth for the HUD. A frame with no 3D scene draw,
   such as a menu or a movie, skips DLSS: the presenter scales up what the
   scene set holds so far and draws the rest of the frame at full size.
4. After present the scene set is bound again.

MSAA, SMAA, RTGI and the G-buffer are off while DLSS is on. A failed
evaluation scales the scene bilinearly instead and is logged once.

Observed 2026-11-23 with motion vectors on, from boot through title, save
select, the intro movie, character select and the island map (9,000 frames):
no frame drew a screen-space quad before its 3D scene, and no 3D draw came
after it. A frame that did would draw its whole scene at full size without
DLSS.

## Adapter

The presenter asks DXGI for the high-performance adapter, so hybrid laptops
render on the discrete GPU (DLSS needs it). The log names the GPU.

## Observed (2026-11-01)

RTX 4060 Laptop GPU, CPU Exhibition on court 2, DLSS Quality: scene
1280x720 to 1920x1080, 18 jitter phases, a steady 60 FPS, no evaluation
errors. HUD portraits, score and "Replay" are sharp; the net mesh and ropes
reconstruct cleanly. The girls and ball carry their own motion vectors.
