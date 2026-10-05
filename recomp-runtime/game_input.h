#ifndef DOAXBV_GAME_INPUT_H
#define DOAXBV_GAME_INPUT_H

#include "runtime.h"

typedef struct {
    uint32_t held, previous, pressed, released, repeated;
    uint8_t pressure[8];
    int16_t axes[4];
    uint32_t repeat_mask;
    int32_t repeat_delay;
} RecompGameInput;

void recomp_game_input_buttons(RecompGameInput *state, uint32_t held, unsigned steps);
void recomp_game_input_update(void);

#endif
