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
    if (recomp_game_pose_display_time(2, 3, 1, 4, true, 9) != 2.25f ||
        recomp_game_pose_display_time(2, 3, 3, 4, true, 9) != 2.75f) {
        fprintf(stderr, "game actor: display pose lost its authored latency\n");
        return 0;
    }
    if (recomp_game_pose_display_time(30, 1, 1, 2, true, 9) != 30 ||
        recomp_game_pose_display_time(2, 3, 1, 2, false, 9) != 9) {
        fprintf(stderr, "game actor: clip loop or change reused a prior cursor\n");
        return 0;
    }
    for (unsigned steps = 1; steps <= 4; steps *= 2) {
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
