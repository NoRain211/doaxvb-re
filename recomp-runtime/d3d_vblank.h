#ifndef DOAXBV_RECOMP_D3D_VBLANK_H
#define DOAXBV_RECOMP_D3D_VBLANK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void recomp_d3d_vblank_reset(void);
void recomp_d3d_wait_vblank(void);
/* Total time spent waiting for guest vblanks, so pass pacing can tell work
   from waiting. */
uint64_t recomp_d3d_vblank_waited_ns(void);

#ifdef __cplusplus
}
#endif

#endif
