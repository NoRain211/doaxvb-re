#ifndef DOAXBV_RECOMP_D3D_DRAW_ADAPTER_H
#define DOAXBV_RECOMP_D3D_DRAW_ADAPTER_H

#include "d3d_draw_model.h"
#include "d3d_presenter.h"
#include "runtime.h"

#include <stdint.h>

void recomp_d3d_draw_adapter_reset(void);

/* Draws submitted to the presenter, and draws the seam declined to submit.
   The runner reports both so a silent decline cannot masquerade as an
   empty scene. */
uint32_t recomp_d3d_draw_adapter_submitted(void);
uint32_t recomp_d3d_draw_adapter_declined(void);

/* Reports every distinct FVF that reached the presenter and its draw count. */
void recomp_d3d_draw_adapter_report_fvf(void);

/* Tags queued commands; logs clear/gamma calls in the adapter order. */
void recomp_d3d_draw_adapter_capture_command(RecompD3dPresenterCommand *command);

/* Completes the optional one-present diagnostic, including an empty frame. */
void recomp_d3d_draw_adapter_capture_present(uint32_t present, uint32_t presenter_error);

RecompFunction recomp_d3d_draw_lookup_manual(uint32_t guest_address);

#endif
