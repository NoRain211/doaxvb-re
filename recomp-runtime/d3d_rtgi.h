#ifndef DOAXBV_RECOMP_D3D_RTGI_H
#define DOAXBV_RECOMP_D3D_RTGI_H

/* Optional hardware ray-traced diffuse GI for the D3D11 presenter. A D3D12
   device with DXR 1.1 runs beside the D3D11 device on the same adapter; the
   D3D11 path stays complete when this is off or unavailable. Every call runs
   on the render worker thread that owns the D3D11 immediate context. */

#include "d3d_presenter.h"

#include <d3d11.h>

struct D3dRtgi;

/* Returns null, after logging why, when D3D12, DXR 1.1 or cross-API sharing is
   unavailable. width x height is the presenter's main render target. */
D3dRtgi *d3d_rtgi_create(ID3D11Device *device, uint32_t width, uint32_t height);
void d3d_rtgi_destroy(D3dRtgi *rtgi);

/* Adds one draw to this frame's scene. Keeps only main-target, depth-writing,
   transformed triangle draws and copies what it needs before returning.
   Returns true when the draw joined the scene. */
bool d3d_rtgi_add_draw(D3dRtgi *rtgi, const RecompD3dPresenterDrawCommand &draw);

/* Traces the recorded scene and adds indirect light to target in place, per
   sample: target = target * (1 + strength * gi). Sky pixels (depth 1) and
   target alpha stay unchanged. The caller runs this before drawing the
   frame's screen-space UI so the UI is not lit.
   target: the main render target (B8G8R8A8_UNORM, width x height, may be
   multisampled).
   color: a resolved copy of target, B8G8R8A8_UNORM, width x height.
   depth: the main depth buffer viewed as R24_UNORM_X8_TYPELESS, a
   Texture2DMS when depth_samples > 1.
   Leaves target unchanged when nothing was recorded or tracing has failed.
   Clears the recorded draws and leaves the context state cleared. */
void d3d_rtgi_apply(D3dRtgi *rtgi, ID3D11DeviceContext *context,
    ID3D11RenderTargetView *target,
    ID3D11ShaderResourceView *color, ID3D11ShaderResourceView *depth,
    uint32_t depth_samples);

#endif
