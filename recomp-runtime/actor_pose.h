#ifndef DOAXBV_RECOMP_ACTOR_POSE_H
#define DOAXBV_RECOMP_ACTOR_POSE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The game's animation clips, as the viewer lists them. Each character has a
   bank of RECOMP_ACTOR_POSE_BANK_SIZE clip pointers loaded for the scene; a
   clip the scene did not load points at the bank's placeholder. A shared bank
   holds a few more, which a clip number names with RECOMP_ACTOR_POSE_SHARED. */
enum {
    RECOMP_ACTOR_POSE_BANK_SIZE = 1000,
    RECOMP_ACTOR_POSE_SHARED = 0x2000,
    RECOMP_ACTOR_POSE_MAX_CLIPS = 2 * RECOMP_ACTOR_POSE_BANK_SIZE,
};

typedef struct RecompActorPoseClips {
    uint16_t clip[RECOMP_ACTOR_POSE_MAX_CLIPS];
    uint32_t count;
} RecompActorPoseClips;

/* Lists the clips present in a character bank, then those in the shared bank,
   given each bank's entries and the placeholder an absent clip points at. An
   entry that is zero or the placeholder is absent. Either bank may be NULL. */
void recomp_actor_pose_list(RecompActorPoseClips *clips,
    const uint32_t *character, uint32_t character_placeholder,
    const uint32_t *shared, uint32_t shared_placeholder);
/* The clip `step` places after (step > 0) or before (step < 0) the given
   one, wrapping; the first clip when the given one is not listed. False when
   empty. */
bool recomp_actor_pose_next(const RecompActorPoseClips *clips, uint16_t clip,
    int step, uint16_t *next, uint32_t *position);

#ifdef __cplusplus
}
#endif

#endif
