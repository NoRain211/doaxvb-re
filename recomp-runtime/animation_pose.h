#ifndef DOAXBV_ANIMATION_POSE_H
#define DOAXBV_ANIMATION_POSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { RECOMP_POSE_CHANNELS = 60 };

typedef struct RecompAnimationPose {
    uint32_t tick, mask, flags;
    float channels[RECOMP_POSE_CHANNELS];
} RecompAnimationPose;

enum RecompAnimationBlendKind {
    RECOMP_ANIMATION_TRANSLATION,
    RECOMP_ANIMATION_EULER,
    RECOMP_ANIMATION_DIRECTION,
    RECOMP_ANIMATION_EYE_DIRECTION,
    RECOMP_ANIMATION_SHOULDER_DIRECTION,
    RECOMP_ANIMATION_WRAPPED_ANGLE
};

/* Caller-supplied executable grouping; no retail table is embedded here. */
typedef struct RecompAnimationGroup {
    uint32_t mask, channel, kind, width;
} RecompAnimationGroup;

/* Preserves the initialized destination's mask and unselected channels, as the
   pose cache does. Inputs remain immutable. Failure leaves output unchanged.
   This is the interpolation mode of AF5C0, including hip-angle quantization. */
#ifdef __cplusplus
extern "C" {
#endif
bool recomp_animation_pose_blend(const RecompAnimationGroup *groups, size_t count,
    const RecompAnimationPose *a, const RecompAnimationPose *b, float weight,
    RecompAnimationPose *output);
#ifdef __cplusplus
}
#endif

#endif
