#include "d3d_split_pose.h"
#include "animation_seam.h"
#include "animation_rotation.h"
#include "d3d_pose_replay.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

double recomp_split_rate(const char *text)
{
    if (!text || !*text) return 0;
    char *end;
    double rate = strtod(text, &end);
    if (*end || !isfinite(rate) || rate < 60 || rate > 1000) return 0;
    return rate;
}

void recomp_split_ball_matrix(const RecompSplitDraw *split, float fraction, float output[16])
{
    float position[3], angles[3];
    for (unsigned i = 0; i < 3; ++i) {
        position[i] = (float)(split->ball_position[0][i]+
            ((double)split->ball_position[1][i]-split->ball_position[0][i])*fraction);
        angles[i] = (float)(split->ball_angles[0][i]+remainder(
            (double)split->ball_angles[1][i]-split->ball_angles[0][i], 6.283185307179586)*fraction);
    }
    if (split->ball == 2) {
        memcpy(output, split->worlds[0], 64);
        memcpy(output+12, position, sizeof position);
        return;
    }
    memcpy(output, split->ball_base.m, 64);
    for (unsigned column = 0; column < 4; ++column)
        output[12+column] = (float)((double)position[0]*output[column]+
            (double)position[1]*output[4+column]+(double)position[2]*output[8+column]+output[12+column]);
    for (int axis = 2; axis >= 0; --axis) {
        unsigned a = (axis+1)%3, b = (axis+2)%3;
        float sine, cosine;
        recomp_animation_sine_cosine(angles[axis], &sine, &cosine);
        for (unsigned column = 0; column < 4; ++column) {
            float x = output[a*4+column], y = output[b*4+column];
            output[a*4+column] = (float)((double)cosine*x+(double)sine*y);
            output[b*4+column] = (float)(-(double)sine*x+(double)cosine*y);
        }
    }
}

bool recomp_d3d_split_draw(const RecompD3dPresenterDrawCommand *source,
    float fraction, const RecompBoneMatrix bones[32], void *vertices,
    RecompD3dPresenterDrawCommand *output)
{
    if (!source || !output || !source->split_pose ||
        source->split_pose_size < sizeof(RecompSplitDraw) ||
        !isfinite(fraction) || fraction < 0 || fraction >= 1) return false;
    const RecompSplitDraw *split = source->split_pose;
    if (!split->count || split->count > 4) return false;
    RecompD3dPoseReplay pose = {0};
    pose.count = split->count;
    memcpy(pose.view, split->view, sizeof pose.view);
    memcpy(pose.projection, split->projection, sizeof pose.projection);
    memcpy(pose.palettes[0], split->worlds, sizeof split->worlds);
    if (split->camera && !recomp_animation_camera_sample(split->cameras, fraction, pose.view, pose.projection)) return false;
    if (split->ball) recomp_split_ball_matrix(split, fraction, pose.palettes[0][0]);
    if (split->pose) {
        if (!bones) return false;
        if (split->joint < 32) memcpy(pose.palettes[0][0], bones[split->joint].m, 64);
        else {
            RecompBoneMatrix palette[4], scratch;
            if (!recomp_animation_build_palette(split->recipe, sizeof split->recipe,
                split->count, bones, 32, split->offsets, 24, &split->initial,
                palette, &scratch)) return false;
            memcpy(pose.palettes[0], palette, split->count*sizeof *palette);
        }
    }
    RecompD3dPresenterDrawCommand draw = *source;
    draw.pose_replay = &pose;
    draw.pose_vertex_bytes = NULL;
    if (split->seam) {
        size_t input_size = (size_t)source->vertex_count*sizeof(RecompSplitVertex);
        if (!vertices || split->seam_source >= 32 || split->seam_destination >= 32 ||
            !bones || source->split_pose_size-sizeof *split < input_size) return false;
        memcpy(vertices, source->vertex_bytes, (size_t)source->vertex_count*source->vertex_stride);
        RecompBoneMatrix relative;
        RecompSeamBasis basis;
        if (!recomp_animation_seam_relative(bones+split->seam_source,
            bones+split->seam_destination, &relative) ||
            !recomp_animation_seam_basis(split->reference, &relative, &basis)) return false;
        const RecompSplitVertex *inputs = (const RecompSplitVertex *)(split+1);
        if (source->vertex_stride < 24) return false;
        for (unsigned i = 0; i < source->vertex_count; ++i) {
            if (!inputs[i].mode) continue;
            RecompBoneMatrix matrix = relative;
            if (inputs[i].mode == 1 && !recomp_animation_seam_matrix(&basis,
                inputs[i].coefficients, &matrix)) return false;
            float vertex[6];
            recomp_animation_seam_vertex(&matrix, inputs[i].position, inputs[i].normal, vertex);
            memcpy((uint8_t *)vertices+(size_t)i*source->vertex_stride, vertex, sizeof vertex);
        }
        draw.vertex_bytes = vertices;
    }
    if (!recomp_d3d_pose_replay_draw(&draw, 0, output)) return false;
    output->pose_replay = NULL;
    return true;
}
