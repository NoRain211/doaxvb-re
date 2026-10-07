#include "game_actor.h"
#include "game_actor_transition.h"
#include "game_display.h"

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
    /* Stops, reversals and rates with unequal display spacing stay bounded. */
    const float positions[] = {0, 1, 1, -1, -1, 0};
    for (unsigned i = 1; i < sizeof positions / sizeof positions[0]; ++i) {
        for (unsigned hz = 60; hz <= 360; ++hz) {
            for (unsigned frame = 0; frame <= hz; ++frame) {
                const float a = positions[i-1], b = positions[i];
                const float value = recomp_game_display_lerp(a, b, (float)frame / hz);
                if (value < fminf(a, b) || value > fmaxf(a, b)) return 0;
            }
        }
    }
    if (recomp_game_display_angle(65530, 6, 0.5f) != 0 ||
        recomp_game_display_lerp(3, 2, -1) != 3 ||
        recomp_game_display_lerp(3, 2, 2) != 2) return 0;
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
