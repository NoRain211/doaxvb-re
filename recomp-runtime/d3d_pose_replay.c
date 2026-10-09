#include "d3d_pose_replay.h"
#include "d3d_draw_model.h"

#include <math.h>
#include <string.h>

static void multiply(const float a[16], const float b[16], float out[16])
{
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column) {
            float sum = 0;
            for (unsigned k = 0; k < 4; ++k) sum += a[row*4+k]*b[k*4+column];
            out[row*4+column] = sum;
        }
}

bool recomp_d3d_pose_replay_draw(const RecompD3dPresenterDrawCommand *source,
    unsigned phase, RecompD3dPresenterDrawCommand *output)
{
    if (!source || !output || !source->pose_replay || phase >= RECOMP_POSE_REPLAY_SAMPLES || source->program_count != 0 ||
        !source->has_transform)
        return false;
    const RecompD3dPoseReplay *pose = source->pose_replay;
    if (pose->count == 0 || pose->count > 4 || pose->count != source->blend_weight_count+1)
        return false;
    RecompD3dPresenterDrawCommand result = *source;
    if (source->pose_vertex_bytes)
        result.vertex_bytes = (const uint8_t *)source->pose_vertex_bytes +
            (size_t)phase*source->vertex_count*source->vertex_stride;
    float world_view[16], view_projection[16];
    for (unsigned i = 0; i < pose->count; ++i)
        for (unsigned j = 0; j < 16; ++j)
            if (!isfinite(pose->palettes[phase][i][j])) return false;
    multiply(pose->view, pose->projection, view_projection);
    multiply(pose->palettes[phase][0], pose->view, world_view);
    multiply(world_view, pose->projection, result.transform);
    for (unsigned i = 0; i < pose->count; ++i) {
        if (i > 0) {
            multiply(pose->palettes[phase][i], view_projection, result.blend_transforms[i-1]);
            for (unsigned j = 0; j < 16; ++j)
                if (!isfinite(result.blend_transforms[i-1][j])) return false;
        }
        memcpy(result.directional.world_transforms[i], pose->palettes[phase][i],
            sizeof result.directional.world_transforms[i]);
        if (result.fog.enabled && result.fog.mode && result.fog.range)
            multiply(pose->palettes[phase][i], pose->view, result.fog_world_view[i]);
        if (result.directional.enabled && !recomp_d3d_normal_transform(
            pose->palettes[phase][i], result.directional.normal_transforms[i])) return false;
    }
    if (result.has_reflection) {
        memcpy(result.reflection_world_view, world_view, sizeof world_view);
        if (!recomp_d3d_normal_transform(world_view, result.reflection_normal)) return false;
    }
    for (unsigned i = 0; i < 16; ++i) if (!isfinite(result.transform[i])) return false;
    *output = result;
    return true;
}
