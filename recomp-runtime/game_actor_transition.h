#ifndef DOAXBV_GAME_ACTOR_TRANSITION_H
#define DOAXBV_GAME_ACTOR_TRANSITION_H

#include "runtime.h"

bool recomp_game_blend_step(uint16_t *age, unsigned *phase, uint16_t duration,
    unsigned steps_per_tick, float *blend);
RecompFunction recomp_game_actor_transition_lookup_manual(uint32_t address);

#endif
