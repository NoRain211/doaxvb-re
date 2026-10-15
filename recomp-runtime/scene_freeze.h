#ifndef DOAXBV_RECOMP_SCENE_FREEZE_H
#define DOAXBV_RECOMP_SCENE_FREEZE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which of the game's tasks keep running while the scene is frozen. The
   game runs its systems as cooperative tasks, one turn each per frame; some
   update the game and some draw it. A frozen scene keeps the tasks that drew
   in any of their last RECOMP_SCENE_FREEZE_HISTORY turns and skips the rest,
   so the picture stays on screen and stops changing. */
enum {
    RECOMP_SCENE_FREEZE_TASKS = 32,
    RECOMP_SCENE_FREEZE_HISTORY = 8,
};

typedef struct RecompSceneFreezeModel {
    /* The function each task slot ran on its last recorded turn. */
    uint32_t function[RECOMP_SCENE_FREEZE_TASKS];
    /* One bit per recent turn, newest lowest: set when it drew. */
    uint8_t drew[RECOMP_SCENE_FREEZE_TASKS];
} RecompSceneFreezeModel;

void recomp_scene_freeze_reset(RecompSceneFreezeModel *model);
/* Records one finished turn of a task. A slot that now runs a different
   function starts its history again. */
void recomp_scene_freeze_note_turn(RecompSceneFreezeModel *model,
    uint32_t slot, uint32_t function, uint32_t draws);
/* True when a frozen scene still runs this task's turn. */
bool recomp_scene_freeze_keeps(const RecompSceneFreezeModel *model,
    uint32_t slot, uint32_t function);

#ifdef __cplusplus
}
#endif

#endif
