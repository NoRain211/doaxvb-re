#include "actor_pose.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(bool condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "actor pose test failed: %s\n", what);
        ++failures;
    }
}

int main(void)
{
    static uint32_t character[RECOMP_ACTOR_POSE_BANK_SIZE], shared[RECOMP_ACTOR_POSE_BANK_SIZE];
    RecompActorPoseClips clips;
    uint16_t next = 0u;
    uint32_t position = 0u;
    const uint32_t empty = 0x8209dfa4u, shared_empty = 0x828bdfa4u;

    for (unsigned i = 0u; i < RECOMP_ACTOR_POSE_BANK_SIZE; ++i) {
        character[i] = empty;
        shared[i] = shared_empty;
    }
    character[0x18] = 0x820a0000u;
    character[0x293] = 0x820e40ecu;
    character[0x2d1] = 0x82110d88u;
    character[0x300] = 0u;
    shared[1] = 0x828bf38cu;

    recomp_actor_pose_list(&clips, character, empty, shared, shared_empty);
    check(clips.count == 4u, "three character clips and one shared clip are listed");
    check(clips.clip[0] == 0x18 && clips.clip[1] == 0x293 && clips.clip[2] == 0x2d1,
        "character clips come first, in clip order");
    check(clips.clip[3] == (RECOMP_ACTOR_POSE_SHARED | 1u), "a shared clip carries the shared flag");

    check(recomp_actor_pose_next(&clips, 0x293, 1, &next, &position) && next == 0x2d1 && position == 2u,
        "next moves forward");
    check(recomp_actor_pose_next(&clips, 0x18, -1, &next, NULL) && next == (RECOMP_ACTOR_POSE_SHARED | 1u),
        "previous wraps to the end");
    check(recomp_actor_pose_next(&clips, RECOMP_ACTOR_POSE_SHARED | 1u, 1, &next, NULL) && next == 0x18,
        "next wraps to the start");
    check(recomp_actor_pose_next(&clips, 0x100, 1, &next, &position) && next == 0x18 && position == 0u,
        "an unlisted clip starts from the first");

    recomp_actor_pose_list(&clips, NULL, 0u, NULL, 0u);
    check(clips.count == 0u && !recomp_actor_pose_next(&clips, 0x18, 1, &next, NULL),
        "no banks, no clips");

    if (failures == 0) printf("actor pose test passed\n");
    return failures == 0 ? 0 : 1;
}
