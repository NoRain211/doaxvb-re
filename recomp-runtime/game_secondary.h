#ifndef RECOMP_GAME_SECONDARY_H
#define RECOMP_GAME_SECONDARY_H

#include "runtime.h"

void recomp_game_secondary_particle(float state[16], const float root[4],
    const float carry[4], const float force[4], float coupling, float damping,
    float gravity, unsigned steps_per_tick);
RecompFunction recomp_game_secondary_lookup_manual(uint32_t address);

#endif
