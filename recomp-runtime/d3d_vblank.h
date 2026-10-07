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

#ifdef __cplusplus
}
#endif

#endif
