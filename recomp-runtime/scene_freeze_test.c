#include "scene_freeze.h"

#include <stdio.h>

static int failures;

static void check(bool condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "scene freeze test failed: %s\n", what);
        ++failures;
    }
}

int main(void)
{
    RecompSceneFreezeModel model;

    recomp_scene_freeze_reset(&model);
    check(!recomp_scene_freeze_keeps(&model, 3u, 0x79d00u), "an unseen task is skipped");

    recomp_scene_freeze_note_turn(&model, 3u, 0x79d00u, 1200u);
    recomp_scene_freeze_note_turn(&model, 4u, 0xa37f0u, 0u);
    check(recomp_scene_freeze_keeps(&model, 3u, 0x79d00u), "a task that drew keeps running");
    check(!recomp_scene_freeze_keeps(&model, 4u, 0xa37f0u), "a task that never drew is skipped");
    check(!recomp_scene_freeze_keeps(&model, 3u, 0xb42b0u), "a slot reused by another function is skipped");

    for (unsigned turn = 1u; turn < RECOMP_SCENE_FREEZE_HISTORY; ++turn) {
        recomp_scene_freeze_note_turn(&model, 3u, 0x79d00u, 0u);
    }
    check(recomp_scene_freeze_keeps(&model, 3u, 0x79d00u), "a draw within the history still counts");
    recomp_scene_freeze_note_turn(&model, 3u, 0x79d00u, 0u);
    check(!recomp_scene_freeze_keeps(&model, 3u, 0x79d00u), "a draw older than the history no longer counts");

    recomp_scene_freeze_note_turn(&model, 3u, 0x79d00u, 5u);
    recomp_scene_freeze_note_turn(&model, 3u, 0xb42b0u, 0u);
    check(!recomp_scene_freeze_keeps(&model, 3u, 0xb42b0u), "a new function starts its history again");

    recomp_scene_freeze_note_turn(&model, RECOMP_SCENE_FREEZE_TASKS, 1u, 1u);
    check(!recomp_scene_freeze_keeps(&model, RECOMP_SCENE_FREEZE_TASKS, 1u), "slots past the table are ignored");

    if (failures == 0) printf("scene freeze test passed\n");
    return failures == 0 ? 0 : 1;
}
