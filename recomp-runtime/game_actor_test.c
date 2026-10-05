#include "game_actor.h"
#include "game_actor_transition.h"

#include <math.h>
#include <stdio.h>

int recomp_game_actor_test(void)
{
    const RecompGameClipAdvance below = recomp_game_clip_advance(0, 0.99999999, 0, 1);
    const RecompGameClipAdvance negative = recomp_game_clip_advance(0, -0.99999999, 0, 1);
    if (below.cursor != 1 || below.frame != 0 || negative.cursor != -1 || negative.frame != 0) {
        fprintf(stderr, "game actor: frame conversion rounded the stored float\n");
        return 0;
    }
    for (unsigned steps = 1; steps <= 4; steps *= 2) {
        float history[4] = {0};
        unsigned position = 0;
        for (unsigned sample = 1; sample <= 60 * steps; ++sample) {
            const float cursor = (float)(sample - 1u) / steps;
            const float observed = recomp_game_pose_sample_time(history, &position,
                cursor, steps, sample == 1);
            const float expected = sample <= steps ? 0 : (float)(sample - steps) / steps;
            if (observed != expected) {
                fprintf(stderr, "game actor: pose sampling lost its authored latency\n");
                return 0;
            }
        }
        if (recomp_game_pose_sample_time(history, &position, 40, steps, true) != 40) {
            fprintf(stderr, "game actor: clip change reused a prior cursor\n");
            return 0;
        }
        uint16_t age = 0;
        unsigned phase = 0;
        float blend = 0, cursor = 0;
        for (unsigned sample = 1; sample <= 30 * steps; ++sample) {
            const bool complete = recomp_game_blend_step(&age, &phase, 30, steps, &blend);
            if (complete != (sample == 30 * steps) ||
                (!complete && fabsf(blend - (float)sample / (30 * steps)) > 1e-6f)) {
                fprintf(stderr, "game actor: transition duration or intermediate pose changed\n");
                return 0;
            }
            cursor = recomp_game_clip_advance(cursor, 1.0 / steps, 0, steps).cursor;
        }
        if (cursor != 30 || age != 30 || phase != 0) {
            fprintf(stderr, "game actor: clip duration depends on sample rate\n");
            return 0;
        }
    }
    return 1;
}
