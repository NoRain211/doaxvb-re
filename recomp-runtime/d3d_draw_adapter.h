#ifndef DOAXBV_RECOMP_D3D_DRAW_ADAPTER_H
#define DOAXBV_RECOMP_D3D_DRAW_ADAPTER_H

#include "d3d_draw_model.h"
#include "runtime.h"

#include <stdint.h>

void recomp_d3d_draw_adapter_reset(void);

/* Draws submitted to the presenter, and draws the seam declined to submit.
   The runner reports both so a silent decline cannot masquerade as an
   empty scene. */
uint32_t recomp_d3d_draw_adapter_submitted(void);
/* 2D draws (pre-transformed vertices) to the back buffer that fall wholly
   inside one of these rectangles, {left, top, right, bottom} in the game's
   screen pixels, are left out. At most RECOMP_D3D_SCREEN_MASKS; a count of
   zero shows everything again. */
enum { RECOMP_D3D_SCREEN_MASKS = 4 };
void recomp_d3d_draw_set_screen_mask(const float rects[][4], uint32_t count);
uint32_t recomp_d3d_draw_adapter_declined(void);

/* Reports every distinct FVF that reached the presenter and its draw count. */
void recomp_d3d_draw_adapter_report_fvf(void);

/* Completes the optional one-present diagnostic, including an empty frame. */
void recomp_d3d_draw_adapter_capture_present(uint32_t present, uint32_t presenter_error);

RecompFunction recomp_d3d_draw_lookup_manual(uint32_t guest_address);

#endif
