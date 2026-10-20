#include "game_world_timers.h"

#include <stdio.h>

int recomp_game_world_test(void)
{
    for (unsigned steps = 1; steps <= 4; steps *= 2) {
        uint32_t phase = 0, angle = 0;
        for (unsigned sample = 1; sample <= 600u * steps; ++sample) {
            angle += recomp_world_timer_split_units(91, steps, &phase);
            if (sample % steps == 0 && angle != 91u * sample / steps) {
                fprintf(stderr, "game world: decorative speed depends on sample rate\n");
                return 0;
            }
        }
    }
    return 1;
}
