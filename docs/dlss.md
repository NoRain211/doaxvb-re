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
   colour, and clears its depth for the HUD.
4. After present the scene set is bound again.

MSAA, SMAA, RTGI and the G-buffer are off while DLSS is on. A failed
evaluation scales the scene bilinearly instead and is logged once.

## Adapter

The presenter now asks DXGI for the high-performance adapter, so hybrid
laptops render on the discrete GPU (DLSS needs it). `RECOMP_D3D_GPU=integrated`
picks the power-saving adapter instead. The log names the adapter.

## Observed (2026-11-01)

RTX 4060 Laptop GPU, CPU Exhibition on court 2, DLSS Quality: scene
1280x720 to 1920x1080, 18 jitter phases, a steady 60 FPS, no evaluation
errors. HUD portraits, score and "Replay" are sharp; the net mesh and ropes
reconstruct cleanly. The girls and ball carry their own motion vectors.
