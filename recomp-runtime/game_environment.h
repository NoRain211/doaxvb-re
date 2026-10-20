#ifndef DOAXBV_GAME_ENVIRONMENT_H
#define DOAXBV_GAME_ENVIRONMENT_H

#include "runtime.h"

#define GAME_ENVIRONMENT_RECORDS 4u
#define GAME_ENVIRONMENT_INPUT_SIZE 0x3cu
#define GAME_ENVIRONMENT_OUTPUT_SIZE 0x58u

typedef struct GameEnvironmentRecord {
    uint8_t input[GAME_ENVIRONMENT_INPUT_SIZE];
    uint8_t output[GAME_ENVIRONMENT_OUTPUT_SIZE];
    uint8_t working[GAME_ENVIRONMENT_OUTPUT_SIZE];
} GameEnvironmentRecord;

typedef struct GameEnvironmentInterpolation {
    uint8_t observed_input[0x38u];
    uint16_t observed_mode;
    uint16_t observed_count;
    unsigned phase;
    bool observed;
    float scalar_start[11];
    float scalar_end[11];
    uint32_t angle_start[2];
    uint32_t angle_end[2];
    int32_t angle_step[2];
} GameEnvironmentInterpolation;

typedef struct GameEnvironmentModel {
    GameEnvironmentRecord records[GAME_ENVIRONMENT_RECORDS];
    GameEnvironmentInterpolation interpolation[GAME_ENVIRONMENT_RECORDS];
} GameEnvironmentModel;

void game_environment_model_step(GameEnvironmentModel *model, unsigned steps_per_tick);

RecompFunction recomp_game_environment_lookup_manual(uint32_t address);

#endif
