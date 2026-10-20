#include "scene_freeze.h"

#include <stddef.h>
#include <string.h>

void recomp_scene_freeze_reset(RecompSceneFreezeModel *model)
{
    if (model != NULL) {
        memset(model, 0, sizeof *model);
    }
}

void recomp_scene_freeze_note_turn(RecompSceneFreezeModel *model,
    uint32_t slot, uint32_t function, uint32_t draws)
{
    if (model == NULL || slot >= RECOMP_SCENE_FREEZE_TASKS) {
        return;
    }
    if (model->function[slot] != function) {
        model->function[slot] = function;
        model->drew[slot] = 0u;
    }
    model->drew[slot] = (uint8_t)((model->drew[slot] << 1) | (draws != 0u ? 1u : 0u));
}

bool recomp_scene_freeze_keeps(const RecompSceneFreezeModel *model,
    uint32_t slot, uint32_t function)
{
    return model != NULL && slot < RECOMP_SCENE_FREEZE_TASKS &&
        model->function[slot] == function && model->drew[slot] != 0u;
}
