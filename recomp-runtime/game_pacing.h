#ifndef DOAXBV_GAME_PACING_H
#define DOAXBV_GAME_PACING_H

#include <stdbool.h>
#include <stdint.h>

/* A negative delay retains elapsed game time until subsequent ticks catch up.
   The presentation-only policy instead discards a missed deadline. */
static inline int64_t recomp_game_pacing_delay_ns(
    int64_t until_previous_deadline, int64_t tick_ns, bool preserve_elapsed)
{
    const int64_t delay = until_previous_deadline + tick_ns;
    return preserve_elapsed || delay > 0 ? delay : 0;
}

#endif
