#ifndef DOAXBV_D3D_SPLIT_POSE_H
#define DOAXBV_D3D_SPLIT_POSE_H

#include "animation_visual.h"
#include "animation_camera.h"
#include "d3d_presenter.h"

typedef struct RecompSplitVertex {
    uint32_t mode;
    float position[3], normal[3], coefficients[4];
} RecompSplitVertex;

/* One allocation: this header followed by vertex_count seam inputs when
   seam is set. Capture owns the allocation; there are no nested pointers. */
typedef struct RecompSplitDraw {
    uint32_t frame, actor, count, joint;
    bool pose, seam, camera;
    unsigned ball;
    float ball_position[2][3], ball_angles[2][3];
    RecompBoneMatrix ball_base;
    RecompVisualPose visual;
    RecompVisualCamera cameras[2];
    uint8_t recipe[16];
    float offsets[24][4];
    RecompBoneMatrix initial;
    float worlds[4][16], view[16], projection[16];
    unsigned seam_source, seam_destination;
    float reference[10][4];
} RecompSplitDraw;

#ifdef __cplusplus
extern "C" {
#endif
double recomp_split_rate(const char *text);
void recomp_split_ball_matrix(const RecompSplitDraw *split, float fraction, float output[16]);
bool recomp_d3d_split_draw(const RecompD3dPresenterDrawCommand *source,
    float fraction, const RecompBoneMatrix bones[32], void *vertices,
    RecompD3dPresenterDrawCommand *output);
#ifdef __cplusplus
}
#endif
#endif
