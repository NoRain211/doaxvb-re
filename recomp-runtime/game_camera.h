#ifndef DOAXBV_GAME_CAMERA_H
#define DOAXBV_GAME_CAMERA_H

#include "runtime.h"

unsigned recomp_game_camera_smoothing_steps(uint32_t camera);

int32_t recomp_game_camera_angle_step(int32_t units, unsigned steps, int32_t *remainder);
RecompFunction recomp_game_camera_lookup_manual(uint32_t address);

float recomp_game_camera_scalar_model(float current, float target, float deadzone,
    float divisor, float limit, float disabled_divisor, unsigned steps);
uint32_t recomp_game_camera_angle_model(uint32_t current, uint32_t target,
    int32_t deadzone, float divisor, int32_t limit, float disabled_divisor, unsigned steps);
RecompFunction recomp_game_camera_smoothing_lookup_manual(uint32_t address);

RecompFunction recomp_game_camera_timers_lookup_manual(uint32_t address);

#endif
