#ifndef DOAXBV_RECOMP_XAPI_TIME_ADAPTER_H
#define DOAXBV_RECOMP_XAPI_TIME_ADAPTER_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

uint64_t recomp_xapi_performance_counter(void);
uint64_t recomp_xapi_performance_frequency(void);
/* Host time in nanoseconds for pacing, never frame-locked. */
uint64_t recomp_xapi_host_ns(void);
/* Frame-locked guest time for rollback. While enabled, each frame
   boundary advances the counter one 60 Hz frame, so replaying the same
   frames reads the same times. RECOMP_FRAME_CLOCK=1 enables it at startup. */
typedef struct RecompFrameClockState {
    uint64_t base_ns;
    uint64_t offset_ns;
    uint64_t frame;
    bool enabled;
} RecompFrameClockState;
bool recomp_xapi_frame_clock_enabled(void);
/* 2003-01-01 00:00 UTC as a FILETIME: the date netplay runners share, since
   host dates and file times differ between machines. */
#define RECOMP_FRAME_CLOCK_EPOCH_FILETIME UINT64_C(126858528000000000)
void recomp_xapi_frame_clock_set_enabled(bool enabled);
void recomp_xapi_frame_clock_reset(void);
void recomp_xapi_frame_clock_advance(void);
void recomp_xapi_frame_clock_save(RecompFrameClockState *state);
void recomp_xapi_frame_clock_restore(const RecompFrameClockState *state);
RecompFunction recomp_xapi_time_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
