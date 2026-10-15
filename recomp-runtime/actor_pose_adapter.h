#ifndef DOAXBV_RECOMP_ACTOR_POSE_ADAPTER_H
#define DOAXBV_RECOMP_ACTOR_POSE_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Picks the next active character to pose. */
void recomp_actor_pose_next_actor(void);
/* What the pose controls are doing, for the overlay. */
typedef struct RecompActorPoseStatus {
    uint32_t actor;
    bool active;
    bool holding;
    uint16_t clip;
    /* The held clip's place in the list of clips, from 1, and the list's
       length; zero when nothing is held. */
    uint32_t position, count;
} RecompActorPoseStatus;
void recomp_actor_pose_status(RecompActorPoseStatus *status);
/* The character's name when the game names her (in a match), else
   "Character n". */
void recomp_actor_pose_name(uint32_t actor, char *name, uint32_t size);
/* Chooses a character to pose by number. */
void recomp_actor_pose_choose(uint32_t actor);
/* Characters whose bit is set are moved far below the ground. */
void recomp_actor_pose_set_hidden(uint32_t mask);
/* Where the chosen character's hips are, for the camera to face. */
bool recomp_actor_pose_focus(float point[3]);
/* The level direction the chosen character's hips face. */
bool recomp_actor_pose_facing(float direction[3]);
/* Plays the next (step > 0) or previous clip on the chosen character and
   holds it. True when a clip was requested. */
bool recomp_actor_pose_step_clip(int step);
/* Hands the chosen character's animation back to the game. */
void recomp_actor_pose_release(void);
/* Once a frame, after the game's animation update: keeps a held clip playing
   and the character where it stood. */
void recomp_actor_pose_frame(void);
/* Wraps the skeleton build to keep a held character in place. */
RecompFunction recomp_actor_pose_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
