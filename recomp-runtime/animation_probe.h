#ifndef DOAXBV_ANIMATION_PROBE_H
#define DOAXBV_ANIMATION_PROBE_H

#include "runtime.h"

RecompFunction recomp_animation_probe_lookup_manual(uint32_t address);
void recomp_animation_probe_capture_frame(uint32_t frame);
struct RecompD3dPoseReplay;
struct RecompD3dPresenterDrawCommand;
void recomp_animation_probe_capture_vertices(const struct RecompD3dPresenterDrawCommand *draw,
    const float worlds[4][16], unsigned count);
/* Caller frees the returned host-owned diagnostic sample buffers. */
void *recomp_animation_probe_pose_vertices(const struct RecompD3dPresenterDrawCommand *draw);
bool recomp_animation_probe_pose_replay(const float worlds[4][16], unsigned count,
    struct RecompD3dPoseReplay *output);

#endif
