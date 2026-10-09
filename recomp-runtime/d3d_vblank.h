#ifndef DOAXBV_RECOMP_D3D_VBLANK_H
#define DOAXBV_RECOMP_D3D_VBLANK_H

#ifdef __cplusplus
extern "C" {
#endif

void recomp_d3d_vblank_reset(void);
void recomp_d3d_wait_vblank(void);
// Sleeps the calling thread until a steady_clock time, in nanoseconds since its epoch.
void recomp_d3d_sleep_until(long long steady_ns);

#ifdef __cplusplus
}
#endif

#endif
