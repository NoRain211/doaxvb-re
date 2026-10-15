#include "actor_pose.h"

#include <stddef.h>

static void add_bank(RecompActorPoseClips *clips, const uint32_t *bank,
    uint32_t placeholder, uint16_t flag)
{
    if (bank == NULL) return;
    for (uint32_t id = 0u; id < RECOMP_ACTOR_POSE_BANK_SIZE; ++id) {
        if (bank[id] != 0u && bank[id] != placeholder &&
            clips->count < RECOMP_ACTOR_POSE_MAX_CLIPS) {
            clips->clip[clips->count++] = (uint16_t)(id | flag);
        }
    }
}

void recomp_actor_pose_list(RecompActorPoseClips *clips,
    const uint32_t *character, uint32_t character_placeholder,
    const uint32_t *shared, uint32_t shared_placeholder)
{
    if (clips == NULL) return;
    clips->count = 0u;
    add_bank(clips, character, character_placeholder, 0u);
    add_bank(clips, shared, shared_placeholder, RECOMP_ACTOR_POSE_SHARED);
}

bool recomp_actor_pose_next(const RecompActorPoseClips *clips, uint16_t clip,
    int step, uint16_t *next, uint32_t *position)
{
    if (clips == NULL || clips->count == 0u || next == NULL) return false;
    uint32_t at = clips->count;
    for (uint32_t i = 0u; i < clips->count; ++i) {
        if (clips->clip[i] == clip) {
            at = i;
            break;
        }
    }
    uint32_t chosen;
    if (at == clips->count) {
        chosen = 0u;
    } else if (step > 0) {
        chosen = (at + 1u) % clips->count;
    } else if (step < 0) {
        chosen = (at + clips->count - 1u) % clips->count;
    } else {
        chosen = at;
    }
    *next = clips->clip[chosen];
    if (position != NULL) *position = chosen;
    return true;
}
