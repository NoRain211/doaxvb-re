#ifndef DOAXBV_ANIMATION_VISUAL_H
#define DOAXBV_ANIMATION_VISUAL_H

#include "animation_pose.h"
#include "animation_skeleton.h"

typedef struct RecompVisualPose {
    RecompSkeletonTables tables;
    RecompAnimationGroup groups[24];
    RecompAnimationPose channels[2];
    RecompSkeletonTargets targets[2];
    RecompBoneMatrix endpoints[2][32];
} RecompVisualPose;

#ifdef __cplusplus
extern "C" {
#endif
/* Immutable tick endpoints; only visual root placement and clip channels move.
   Controllers, contacts and secondary simulation do not advance here. */
bool recomp_animation_visual_sample(const RecompVisualPose *input, float fraction,
    RecompBoneMatrix output[32]);
#ifdef __cplusplus
}
#endif
#endif
