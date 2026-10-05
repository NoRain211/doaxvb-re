#ifndef DOAXBV_GAME_SCHEDULER_H
#define DOAXBV_GAME_SCHEDULER_H

#include "runtime.h"

/* Guest sleep counts remain integer 60 Hz ticks. The remainder belongs to
   one sleep, and must be cleared when a slot is disabled or resumes. */
bool recomp_game_sleep_step(int32_t *ticks, unsigned *subtick, unsigned steps);
void recomp_game_scheduler_reset(unsigned hz);
void recomp_game_scheduler_tick(void);
unsigned recomp_game_scheduler_hz(void);
uint32_t recomp_game_debug_rng_state(void);
unsigned recomp_game_requested_hz(void);
bool recomp_game_timing_enabled(void);
void recomp_game_test_clock_begin(bool enabled);
RecompFunction recomp_game_scheduler_lookup_manual(uint32_t address);

#endif
