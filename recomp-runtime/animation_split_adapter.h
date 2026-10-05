#ifndef DOAXBV_ANIMATION_SPLIT_ADAPTER_H
#define DOAXBV_ANIMATION_SPLIT_ADAPTER_H
#include "d3d_split_pose.h"

bool recomp_animation_split_enabled(void);
void recomp_animation_split_capture(unsigned actor, uint32_t frame,
    const RecompVisualPose *snapshot);
void recomp_animation_split_palette(unsigned actor, uint32_t frame, uint32_t object,
    unsigned count, const uint8_t recipe[16], const float offsets[24][4],
    const RecompBoneMatrix *initial, const RecompBoneMatrix original[4]);
void recomp_animation_split_rigid(void);
void recomp_animation_split_finish(unsigned actor, uint32_t frame, float displacement,
    const RecompBoneMatrix bones[32]);
void recomp_animation_split_camera(uint32_t address);
void *recomp_animation_split_draw(const RecompD3dPresenterDrawCommand *draw,
    const float worlds[4][16], unsigned count, const float view[16],
    const float projection[16], uint32_t *size);
#endif
