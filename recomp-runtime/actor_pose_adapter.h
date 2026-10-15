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
/* Where the chosen character's hips are, for the camera to face. */
bool recomp_actor_pose_focus(float point[3]);
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
