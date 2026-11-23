#ifndef DOAXBV_RECOMP_D3D_FSR_H
#define DOAXBV_RECOMP_D3D_FSR_H

/* AMD FSR 1.0 window upscale for the D3D11 presenter (docs/display-modes.md):
   EASU into a letterbox-sized texture, then RCAS into the window. Built only
   when third_party/fsr1 exists (RECOMP_FSR1). Every call runs on the render
   worker thread that owns the D3D11 immediate context. */

#include <d3d11.h>

struct D3dFsr;

/* sharpness 0..1: 1 is RCAS's maximum, 0 two stops softer. Returns null,
   after logging why, without feature level 11_0 or the FSR headers. */
D3dFsr *d3d_fsr_create(ID3D11Device *device, float sharpness);
void d3d_fsr_destroy(D3dFsr *fsr);

/* Upscales source into viewport on output. Returns false, drawing nothing,
   when viewport is not larger than source; FSR only upscales. Leaves the
   context state cleared after drawing. */
bool d3d_fsr_scale(D3dFsr *fsr, ID3D11DeviceContext *context, ID3D11ShaderResourceView *source,
    ID3D11RenderTargetView *output, const D3D11_VIEWPORT &viewport);

#endif
