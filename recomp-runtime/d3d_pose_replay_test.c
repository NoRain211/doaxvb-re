#include "d3d_pose_replay.h"

#include <float.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Replay line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static void identity(float m[16])
{
    memset(m, 0, 16*sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1;
}

int main(void)
{
    RecompD3dPoseReplay pose = {0};
    RecompD3dPresenterDrawCommand source = {0}, result = {0};
    pose.count = 2;
    identity(pose.view); identity(pose.projection);
    pose.view[13] = 3; pose.projection[0] = 2;
    for (unsigned phase = 0; phase < RECOMP_POSE_REPLAY_SAMPLES; ++phase)
        for (unsigned i = 0; i < 2; ++i) {
            identity(pose.palettes[phase][i]);
            pose.palettes[phase][i][12] = (float)(phase+i);
        }
    float vertices[3] = {0};
    source.pose_replay = &pose;
    source.vertex_bytes = vertices;
    source.blend_weight_count = 1;
    source.has_transform = true;
    source.has_reflection = true;
    source.directional.enabled = true;
    RecompD3dPresenterDrawCommand before = source;
    for (unsigned phase = 0; phase < RECOMP_POSE_REPLAY_SAMPLES; ++phase) {
        CHECK(recomp_d3d_pose_replay_draw(&source, phase, &result));
        CHECK(result.transform[12] == phase*2 && result.transform[13] == 3);
        CHECK(result.blend_transforms[0][12] == (phase+1)*2);
        CHECK(result.reflection_world_view[12] == phase);
        CHECK(result.directional.normal_transforms[0][0] == 1);
        CHECK(result.directional.normal_transforms[1][5] == 1);
        CHECK(result.reflection_normal[10] == 1);
        CHECK(result.vertex_bytes == vertices);
        CHECK(memcmp(&source, &before, sizeof source) == 0);
    }
    float sampled_vertices[RECOMP_POSE_REPLAY_SAMPLES][3] = {{0}};
    source.pose_vertex_bytes = sampled_vertices;
    source.vertex_count = 1; source.vertex_stride = 3*sizeof(float);
    for (unsigned phase = 0; phase < RECOMP_POSE_REPLAY_SAMPLES; ++phase) {
        CHECK(recomp_d3d_pose_replay_draw(&source, phase, &result));
        CHECK(result.vertex_bytes == sampled_vertices[phase]);
    }
    RecompD3dPresenterDrawCommand saved = result;
    CHECK(!recomp_d3d_pose_replay_draw(&source, RECOMP_POSE_REPLAY_SAMPLES, &result));
    CHECK(memcmp(&result, &saved, sizeof result) == 0);
    source.has_transform = false;
    CHECK(!recomp_d3d_pose_replay_draw(&source, 1, &result));
    CHECK(memcmp(&result, &saved, sizeof result) == 0);
    source.has_transform = true;
    /* Only a secondary palette overflows; the primary transform stays finite. */
    pose.palettes[1][1][0] = FLT_MAX;
    CHECK(!recomp_d3d_pose_replay_draw(&source, 1, &result));
    CHECK(memcmp(&result, &saved, sizeof result) == 0);
    pose.palettes[1][1][0] = 1;
    source.program_count = 1;
    CHECK(!recomp_d3d_pose_replay_draw(&source, 1, &result));
    CHECK(memcmp(&result, &saved, sizeof result) == 0);
    source.program_count = 0;
    pose.count = 3;
    CHECK(!recomp_d3d_pose_replay_draw(&source, 1, &result));
    CHECK(memcmp(&result, &saved, sizeof result) == 0);
    pose.count = 2;
    pose.palettes[1][1][0] = NAN;
    CHECK(!recomp_d3d_pose_replay_draw(&source, 1, &result));
    CHECK(memcmp(&result, &saved, sizeof result) == 0);
    pose.count = 1;
    source.blend_weight_count = 0;
    CHECK(recomp_d3d_pose_replay_draw(&source, 1, &result));
    CHECK(result.transform[12] == 2 && result.transform[13] == 3);
    puts("Pose replay transform tests passed");
    return 0;
}
