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
   frames reads the same times. Whoever enables it advances it at its frame
   boundary. */
bool recomp_xapi_frame_clock_enabled(void);
void recomp_xapi_frame_clock_set_enabled(bool enabled);
void recomp_xapi_frame_clock_reset(void);
void recomp_xapi_frame_clock_advance(void);
/* Rollback snapshots of the clock's time. Whether it runs is not part of
   them, so a restore cannot turn it on or off. */
size_t recomp_xapi_frame_clock_state_size(void);
void recomp_xapi_frame_clock_state_save(void *state);
void recomp_xapi_frame_clock_state_restore(const void *state);
/* The system time as a FILETIME. While frame-locked, netplay runners read
   the same date, since host dates differ between machines. */
uint64_t recomp_xapi_system_filetime(void);
/* A host file's FILETIME as the guest sees it; a fixed date while frame-locked. */
uint64_t recomp_xapi_guest_file_time(uint64_t host_filetime);
RecompFunction recomp_xapi_time_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
