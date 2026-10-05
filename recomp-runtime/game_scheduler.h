#ifndef DOAXBV_GAME_SCHEDULER_H
#define DOAXBV_GAME_SCHEDULER_H

#include "runtime.h"

/* Guest sleep counts remain integer 60 Hz ticks. The remainder belongs to
   one sleep, and must be cleared when a slot is disabled or resumes. */
bool recomp_game_sleep_step(int32_t *ticks, unsigned *half_tick, unsigned hz);
void recomp_game_scheduler_reset(unsigned hz);
void recomp_game_scheduler_tick(void);
RecompFunction recomp_game_scheduler_lookup_manual(uint32_t address);

#endif
