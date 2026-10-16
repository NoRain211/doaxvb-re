#ifndef DOAXBV_RECOMP_OVERLAY_D3D11_H
#define DOAXBV_RECOMP_OVERLAY_D3D11_H

#include <d3d11.h>

#include "overlay_text.h"

/* Draws the runtime's overlay text (overlay_text.h) with Dear ImGui into the
   window's back buffer after the game's frame, on the presenter's thread.
   Created on first use; nothing is drawn while the overlay is empty. */
void recomp_overlay_d3d11_draw(ID3D11Device *device, ID3D11DeviceContext *context,
    ID3D11RenderTargetView *target, UINT width, UINT height, const RecompOverlayText *text);
void recomp_overlay_d3d11_shutdown(void);

#endif
