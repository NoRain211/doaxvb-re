#ifndef DOAXBV_D3D_POSE_REPLAY_H
#define DOAXBV_D3D_POSE_REPLAY_H

#include "d3d_presenter.h"

/* Diagnostic quarter samples only; this count does not define a present rate. */
enum { RECOMP_POSE_REPLAY_SAMPLES = 5 };

/* Owned by the capture packet after submit. Numeric provenance comes from the
   game palette/object binding, not draw order or vertex-buffer addresses. */
typedef struct RecompD3dPoseReplay {
    uint32_t frame, actor, recipe, count;
    float palettes[RECOMP_POSE_REPLAY_SAMPLES][4][16];
    float view[16], projection[16];
} RecompD3dPoseReplay;

#ifdef __cplusplus
extern "C" {
#endif
bool recomp_d3d_pose_replay_draw(const RecompD3dPresenterDrawCommand *source,
    unsigned phase, RecompD3dPresenterDrawCommand *output);
#ifdef __cplusplus
}
#endif

#endif
