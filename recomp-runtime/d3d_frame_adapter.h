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
/* Runs once a Swap on the game's thread, after the frame is presented, when set. */
extern void (*recomp_d3d_swap_hook)(void);
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
size_t recomp_d3d_frame_adapter_state_size(void);
void recomp_d3d_frame_adapter_state_save(void *state);
void recomp_d3d_frame_adapter_state_restore(const void *state);
/* While any reason holds it, Swap still counts frames, waits for the display
   (except for a rollback replay, below) and presents the frame's work, but
   the window shows nothing new, so the last picture stays on screen. */
enum {
    RECOMP_PRESENT_HOLD_SCENE = 1u,
    RECOMP_PRESENT_HOLD_VIEWER = 2u,
    /* A rollback replay: no vblank wait either, so the frames run unpaced. */
    RECOMP_PRESENT_HOLD_ROLLBACK = 4u,
};
void recomp_d3d_frame_adapter_hold_present(uint32_t reason, bool hold);
/* Shows the last presented frame again with the current overlay screen,
   for a notice while the game waits at a frame boundary. Guest state is
   untouched. */
void recomp_d3d_frame_adapter_present_again(void);
/* Called at the first Clear of each frame, from the guest's drawing, where
   the hook may call the game's own code (leaving the registers as they
   were). */
typedef void (*RecompD3dFrameHook)(void);
void recomp_d3d_frame_adapter_set_first_clear_hook(RecompD3dFrameHook hook);
void recomp_d3d_clear_adapter(void);
void recomp_d3d_swap_adapter(void);
RecompFunction recomp_d3d_frame_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
