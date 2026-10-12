#ifndef DOAXBV_RECOMP_D3D_FRAME_ADAPTER_H
#define DOAXBV_RECOMP_D3D_FRAME_ADAPTER_H

#include "d3d_presenter.h"
#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
/* Safe from the console control thread; cleanup occurs at a frame boundary. */
void recomp_d3d_frame_adapter_request_console_break(void);
#endif
void recomp_d3d_frame_adapter_initialize(
    const RecompD3dPresenterConfig *config,
    uint32_t device_address);
void recomp_d3d_frame_adapter_reset(void);
/* Applies the normal host-window exit to any presenter operation. */
void recomp_d3d_frame_adapter_exit_if_closed(RecompD3dPresenterError error);
void recomp_d3d_frame_adapter_reset_buffers(void);
/* The presenter created by this adapter, or NULL before initialization.
   The draw seam submits through the same presenter so clear, draw, and
   present stay ordered on one owning thread. */
RecompD3dPresenter *recomp_d3d_frame_adapter_presenter(void);
bool recomp_d3d_frame_adapter_target(RecompD3dPresenterTarget *target);
/* The guest's swap counter, so seams that run per draw can tell which frame
   they are in. Diagnostics only; nothing in the frame path depends on it. */
uint32_t recomp_d3d_frame_adapter_swap_counter(void);
void recomp_d3d_clear_adapter(void);
void recomp_d3d_swap_adapter(void);
RecompFunction recomp_d3d_frame_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
