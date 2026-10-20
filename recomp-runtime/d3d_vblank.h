#ifndef DOAXBV_RECOMP_D3D_VBLANK_H
#define DOAXBV_RECOMP_D3D_VBLANK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void recomp_d3d_vblank_reset(void);
void recomp_d3d_wait_vblank(void);
// Sleeps the calling thread until a steady_clock time, in nanoseconds since its epoch.
void recomp_d3d_sleep_until(long long steady_ns);
/* Total time spent waiting for guest vblanks, so pass pacing can tell work
   from waiting. */
uint64_t recomp_d3d_vblank_waited_ns(void);

/* Whole-game pacing shared by the game thread and the render worker. */
void recomp_d3d_count_passes(unsigned ticks, unsigned displays, unsigned dropped);
/* Returns and clears the counts as {ticks, display passes, dropped passes}. */
void recomp_d3d_take_passes(unsigned counts[3]);
/* Render cost of the last measured frame: the larger of worker CPU and GPU time. */
void recomp_d3d_set_frame_cost_ns(uint32_t ns);
uint32_t recomp_d3d_frame_cost_ns(void);
/* Frames per second the game presents (60 until set); each holds refresh/rate. */
void recomp_d3d_set_display_rate(unsigned hz);
unsigned recomp_d3d_display_rate(void);

#ifdef __cplusplus
}
#endif

#endif
