#ifndef DOAXBV_RECOMP_D3D_DLSS_H
#define DOAXBV_RECOMP_D3D_DLSS_H

/* NVIDIA DLSS Super Resolution through dlss_bridge.dll (docs/dlss.md). This
   repository holds only the bridge's C interface, dlss_bridge.h; the runner
   loads the DLL at run time. Every call runs on the render worker thread that
   owns the D3D11 immediate context. */

#include "dlss_bridge.h"

#include <cstdint>

struct D3dDlss;

/* Loads the bridge beside the runner and prepares mode (DlssBridgeMode) for an
   output_width x output_height result. Sets the scene size DLSS wants as its
   input. Returns null after logging why. */
D3dDlss *d3d_dlss_create(ID3D11Device *device, ID3D11DeviceContext *context, int mode,
    uint32_t output_width, uint32_t output_height, uint32_t *render_width, uint32_t *render_height);
void d3d_dlss_destroy(D3dDlss *dlss);

/* Reconstructs frame (every field but output and frame_ms) and draws the
   result over target, an output-sized colour target. A failed evaluation
   draws scene, the input colour, scaled bilinearly instead and is logged
   once. Leaves the context state cleared. */
void d3d_dlss_resolve(D3dDlss *dlss, ID3D11DeviceContext *context, DlssBridgeFrame frame,
    ID3D11ShaderResourceView *scene, ID3D11RenderTargetView *target);

#endif
