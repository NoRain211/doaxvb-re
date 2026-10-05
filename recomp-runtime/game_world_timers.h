#ifndef DOAXBV_GAME_WORLD_TIMERS_H
#define DOAXBV_GAME_WORLD_TIMERS_H

#include <stdint.h>

#include "runtime.h"

static inline uint32_t recomp_world_timer_split_units(
    uint32_t authored_units,
    unsigned steps_per_tick,
    uint32_t *remainder)
{
    if (steps_per_tick == 0u) steps_per_tick = 1u;
    const uint64_t total = (uint64_t)*remainder + authored_units;
    const uint32_t increment = (uint32_t)(total / steps_per_tick);
    *remainder = (uint32_t)(total % steps_per_tick);
    return increment;
}

static inline float recomp_world_timer_lerp(float start, float end, float amount)
{
    return start + (end - start) * amount;
}

void recomp_game_world_clock_motion_model(unsigned steps_per_tick);
void recomp_game_decorative_rates_model(unsigned steps_per_tick);
RecompFunction recomp_game_world_timers_lookup_manual(uint32_t address);

#endif
