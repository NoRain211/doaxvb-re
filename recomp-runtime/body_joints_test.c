/* body_joints: the interpolation table against the game's bytes (0x00333770),
   and the helpers' arithmetic on small cases. */
#include "body_joints.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

/* The game's table, as stored in the XBE at 0x00333770. */
static const float table[16][4] = {
    {1.0f, -0.0f, 0.0f, 0.0f}, {0.8203125f, -0.0546875f, 0.234375f, 0.0f},
    {0.65625f, -0.09375f, 0.4375f, 0.0f}, {0.5078125f, -0.1171875f, 0.609375f, 0.0f},
    {0.375f, -0.125f, 0.75f, 0.0f}, {0.2578125f, -0.1171875f, 0.859375f, 0.0f},
    {0.15625f, -0.09375f, 0.9375f, 0.0f}, {0.0703125f, -0.0546875f, 0.984375f, 0.0f},
    {-0.0f, 0.0f, 1.0f, 0.0f}, {-0.0546875f, 0.0703125f, 0.984375f, 0.0f},
    {-0.09375f, 0.15625f, 0.9375f, 0.0f}, {-0.1171875f, 0.2578125f, 0.859375f, 0.0f},
    {-0.125f, 0.375f, 0.75f, 0.0f}, {-0.1171875f, 0.5078125f, 0.609375f, 0.0f},
    {-0.09375f, 0.65625f, 0.4375f, 0.0f}, {-0.0546875f, 0.8203125f, 0.234375f, 0.0f},
};

int main(void)
{
    for (unsigned k = 0; k < 16; ++k) {
        float w[4];
        recomp_joint_weights(k, w);
        CHECK(memcmp(w, table[k], sizeof w) == 0);
    }
    /* Identity matrix: rotate, transform and place pass values through. */
    const float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float v[8] = {0.25f, -2.0f, 3.5f, 0, 0, 1, 0, 0};
    float a[4], b[4];
    recomp_joint_transform(id, v, a);
    CHECK(a[0] == 0.25f && a[1] == -2.0f && a[2] == 3.5f && a[3] == 1.0f);
    recomp_joint_place(id, v, a, b);
    CHECK(a[2] == 3.5f && b[1] == 1.0f && b[3] == 0.0f);
    float m[16];
    recomp_joint_multiply(m, id, id);
    CHECK(memcmp(m, id, sizeof m) == 0);
    /* The game's square root is within a few ulps of the true one. */
    CHECK(fabsf(recomp_joint_sqrt(2.0f) - 1.41421356f) < 1e-6f);
    /* A frame from the parent and child frames, both identity, with zero
       origins: every blend is the identity. */
    float data[40] = {0};
    for (unsigned i = 0; i < 3; ++i) { data[i * 8 + i] = 1.0f; data[i * 8 + 4 + i] = 1.0f; }
    float frames[64] = {0}, rows[16];
    recomp_joint_frames(id, data, frames);
    for (unsigned i = 0; i < 3; ++i) CHECK(frames[i * 16 + 8 + i] == 1.0f);
    for (unsigned k = 0; k < 16; ++k) {
        float w[4];
        recomp_joint_weights(k, w);
        recomp_joint_blend_frame(frames, w, rows);
        for (unsigned i = 0; i < 3; ++i) CHECK(fabsf(rows[i * 4 + i] - 1.0f) < 1e-6f);
    }
    if (failures) return 1;
    printf("body joints: ok\n");
    return 0;
}
