#include "game_camera.h"
#include "game_scheduler.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

static int check_camera_publication(void)
{
    static uint8_t cameras[0x4a40], actors[0xc40], stack[256], match;
    const RecompMemoryRegion regions[] = {
        {0x0041a800u, sizeof cameras, cameras},
        {0x004d6700u, sizeof actors, actors},
        {0x005d89b8u, 1, &match},
        {0x01000000u, sizeof stack, stack},
    };
    RecompFunction publish = recomp_game_camera_lookup_manual(0x0002f070u);
    if (publish == NULL) return 0;
    for (unsigned variant = 0; variant < 2; ++variant) {
        for (unsigned steps = 1; steps <= 4; steps *= 2) {
            memset(cameras, 0, sizeof cameras);
            memset(actors, 0, sizeof actors);
            recomp_runtime_init(regions, 4, NULL, 0, NULL, 0);
            recomp_game_scheduler_reset(60 * steps);
            recomp_runtime.registers.eax = 0x0041a800u;
            recomp_runtime.registers.esp = 0x010000e0u;
            publish(); /* Inactive cameras discard the preceding run's history. */
            *recomp_memory(0x0041b2d8u, 1) = 1;
            *recomp_memory(0x0041b2ccu, 1) = variant ? 8 : 1;
            for (unsigned sample = 1; sample <= 60 * steps; ++sample) {
                const float staged = (float)(sample - 1u) / steps;
                recomp_guest_store(0x0041a980u, &staged, sizeof staged);
                *recomp_memory_u32(0x0041b154u) = sample / steps;
                recomp_runtime.registers.eax = 0x0041a800u;
                recomp_runtime.registers.esp = 0x010000e0u;
                publish();
                float observed;
                recomp_guest_load(&observed, 0x0041a800u, sizeof observed);
                const float expected = sample <= steps ? 0 : (float)(sample - steps) / steps;
                if (observed != expected || recomp_runtime.registers.esp != 0x010000e4u) {
                    fprintf(stderr, "game camera: publication latency or stack changed\n");
                    recomp_runtime_init(NULL, 0, NULL, 0, NULL, 0);
                    return 0;
                }
            }
        }
    }
    recomp_runtime_init(NULL, 0, NULL, 0, NULL, 0);
    recomp_game_scheduler_reset(60);
    return 1;
}

int recomp_game_camera_test(void)
{
    if (recomp_game_camera_scalar_model(10, 11, 0, 10, 0.5f, 0, 1) != 10.1f ||
        recomp_game_camera_scalar_model(10, 9, 0, 10, 0.5f, 0, 1) != 9.9f ||
        recomp_game_camera_angle_model(0, 17, 0, 32, 728, 0, 1) != 0 ||
        recomp_game_camera_angle_model(0, 33, 0, 32, 728, 0, 1) != 1 ||
        recomp_game_camera_angle_model(0, 65503, 0, 32, 728, 0, 1) != 65535) {
        fprintf(stderr, "game camera: legacy smoothing arithmetic changed\n");
        return 0;
    }
    for (unsigned steps = 1; steps <= 4; steps *= 2) {
        float limited = 0;
        for (unsigned i = 0; i < steps; ++i) {
            limited = recomp_game_camera_scalar_model(limited, 100, 0, 1, 0.5f, 0, steps);
        }
        if (fabsf(limited - 0.5f) > 1e-6f) {
            fprintf(stderr, "game camera: movement limit depends on sample rate\n");
            return 0;
        }
        for (int direction = -1; direction <= 1; direction += 2) {
            int32_t remainder = 0, total = 0;
            for (unsigned sample = 1; sample <= 300 * steps; ++sample) {
                total += recomp_game_camera_angle_step(direction * 18, steps, &remainder);
                if (sample % steps == 0 &&
                    (total != direction * 18 * (int32_t)(sample / steps) || remainder != 0)) {
                    fprintf(stderr, "game camera: orbit speed depends on sample rate\n");
                    return 0;
                }
            }
        }
    }
    return check_camera_publication();
}
