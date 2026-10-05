#include "game_secondary.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

int recomp_game_secondary_test(void)
{
    const float root[4] = {0}, carry[4] = {0.01f, 0, 0.005f, 0};
    const float force[4] = {0, 0, 0.002f, 0};
    const float damping[] = {0.7f, 1, 0};
    for (unsigned d = 0; d < 3; ++d) {
        float reference[16] = {0};
        reference[8] = 0.05f;
        for (unsigned tick = 0; tick < 20; ++tick) {
            recomp_game_secondary_particle(reference, root, carry, force,
                1, damping[d], 0.02f, 1);
        }
        for (unsigned steps = 2; steps <= 4; steps *= 2) {
            float candidate[16] = {0};
            candidate[8] = 0.05f;
            for (unsigned sample = 0; sample < 20 * steps; ++sample) {
                recomp_game_secondary_particle(candidate, root, carry, force,
                    1, damping[d], 0.02f, steps);
            }
            for (unsigned i = 4; i < 12; ++i) {
                if (fabsf(candidate[i] - reference[i]) > 0.00002f) {
                    fprintf(stderr, "secondary physics: constant-force endpoint depends on tick length\n");
                    return 0;
                }
            }
        }
    }
    return 1;
}
