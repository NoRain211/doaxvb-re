#ifndef DOAXBV_ANIMATION_SKELETON_H
#define DOAXBV_ANIMATION_SKELETON_H

#include "animation_palette.h"

enum { RECOMP_POSE_CHANNELS = 60, RECOMP_RIG_OFFSETS = 24, RECOMP_POSE_BONES = 32 };

/* Indices refer to the decoded channel array, not the three-word clip header.
   This is the 24-byte limb description, with the executable's fields named. */
typedef struct RecompLimbTable {
    uint8_t end, parent, proximal, middle, tip;
    uint8_t position_channels[3], rotation_channels[3], pole_channels[2], tip_channel;
    int8_t proximal_quarters[3], middle_quarters[3];
    uint8_t target_mask, positive_bend, upper_body, reserved;
} RecompLimbTable;

typedef struct RecompSkeletonTables {
    float offsets[RECOMP_RIG_OFFSETS][4];
    float alternate_offsets[RECOMP_RIG_OFFSETS][4];
    RecompLimbTable limbs[4];
} RecompSkeletonTables;

typedef struct RecompLimbLengths {
    float proximal, distal, squared_difference;
    float alternate_proximal, alternate_distal, alternate_squared_difference;
} RecompLimbLengths;

/* Controller results are frozen at a simulation tick. Evaluating this input
   never advances eye, look-at, contact, root-motion or morph controllers.
   Ground heights are sampled by the caller at hip, neck and four IK targets. */
typedef struct RecompSkeletonTargets {
    float position[3], heading, ground_height;
    float hip_height, neck_height, limb_height[4];
    RecompLimbLengths lengths[4];
    float target_offset[3], target_weight, upper_morph, lower_morph;
    float look_target[3], look_weight, eyes[4];
    uint8_t target_mask, minimum_bend, terrain_disabled, derived_blend;
} RecompSkeletonTargets;

/* Row-vector world-space bone matrices, before per-draw palette recipes.
   Immutable inputs; malformed/singular input returns false without changing
   output. Tables contain no retail defaults and must be supplied by the caller. */
bool recomp_animation_solve_skeleton(const RecompSkeletonTables *tables,
    const float channels[RECOMP_POSE_CHANNELS], const RecompSkeletonTargets *targets,
    RecompBoneMatrix output[RECOMP_POSE_BONES]);

#endif
