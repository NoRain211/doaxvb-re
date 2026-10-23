#ifndef DOAXBV_RECOMP_D3D_DRAW_ADAPTER_H
#define DOAXBV_RECOMP_D3D_DRAW_ADAPTER_H

#include "d3d_draw_model.h"
#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

void recomp_d3d_draw_adapter_reset(void);

/* Draws submitted to the presenter, and draws the seam declined to submit.
   The runner reports both so a silent decline cannot masquerade as an
   empty scene. */
uint32_t recomp_d3d_draw_adapter_submitted(void);
/* Back-buffer draws a hidden tick skipped. */
uint32_t recomp_d3d_draw_adapter_hidden(void);
/* 2D draws (pre-transformed vertices) to the back buffer that fall wholly
   inside one of these rectangles, {left, top, right, bottom} in the game's
   screen pixels, are left out. At most RECOMP_D3D_SCREEN_MASKS; a count of
   zero shows everything again. */
enum { RECOMP_D3D_SCREEN_MASKS = 4 };
void recomp_d3d_draw_set_screen_mask(const float rects[][4], uint32_t count);
/* Indexed draws from vertex buffer objects at guest addresses in
   [first, end) are left out of the host frame; first == end shows them. */
void recomp_d3d_draw_set_hidden_buffers(uint32_t first, uint32_t end);
uint32_t recomp_d3d_draw_adapter_declined(void);
/* Called with the view transform of each indexed draw submitted, so a
   caller can tell which camera the scene is drawn with; NULL stops it. */
typedef void (*RecompD3dDrawViewObserver)(const float view[16]);
void recomp_d3d_draw_set_view_observer(RecompD3dDrawViewObserver observer);
/* When the device's view transform is `old_view`, puts `view` and
   `projection` in its place, as D3DDevice_SetTransform would. True when it
   did. */
bool recomp_d3d_draw_replace_view(const float old_view[16], const float view[16],
    const float projection[16]);

/* Reports every distinct FVF that reached the presenter and its draw count. */
void recomp_d3d_draw_adapter_report_fvf(void);

/* Completes the optional one-present diagnostic, including an empty frame. */
void recomp_d3d_draw_adapter_capture_present(uint32_t present, uint32_t presenter_error);

/* Called with each draw or clear the presenter accepted, in order; NULL disables. */
struct RecompD3dPresenterCommand;
typedef void (*RecompD3dDrawObserver)(const struct RecompD3dPresenterCommand *command);
void recomp_d3d_draw_adapter_observe(RecompD3dDrawObserver observer);
/* Reports a command submitted outside the draw adapter, such as a clear. */
void recomp_d3d_draw_adapter_notify(const struct RecompD3dPresenterCommand *command);

RecompFunction recomp_d3d_draw_lookup_manual(uint32_t guest_address);

#endif
