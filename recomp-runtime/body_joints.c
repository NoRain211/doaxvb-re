#include "body_joints.h"

#include <math.h>
#include <string.h>

void recomp_joint_multiply(float out[16], const float b[16], const float c[16])
{
    float r[16];
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned lane = 0; lane < 4; ++lane) {
            const float *v = b + row * 4u;
            float sum = v[0] * c[lane] + v[1] * c[4 + lane];
            sum = sum + v[2] * c[8 + lane];
            r[row * 4u + lane] = sum + v[3] * c[12 + lane];
        }
    memcpy(out, r, sizeof r);
}

void recomp_joint_rotate(const float m[16], const float v[3], float out[4])
{
    float r[4];
    for (unsigned lane = 0; lane < 4; ++lane) {
        const float xy = v[0] * m[lane] + v[1] * m[4 + lane];
        r[lane] = xy + v[2] * m[8 + lane];
    }
    memcpy(out, r, sizeof r);
}

void recomp_joint_transform(const float m[16], const float p[3], float out[4])
{
    float r[4];
    for (unsigned lane = 0; lane < 4; ++lane) {
        const float xy = p[0] * m[lane] + p[1] * m[4 + lane];
        const float zw = p[2] * m[8 + lane] + m[12 + lane];
        r[lane] = xy + zw;
    }
    memcpy(out, r, sizeof r);
}

void recomp_joint_place(const float m[16], const float vertex[8], float position[4], float normal[4])
{
    float p[4], n[4];
    for (unsigned lane = 0; lane < 4; ++lane) {
        const float xyz = (vertex[0] * m[lane] + vertex[1] * m[4 + lane]) + vertex[2] * m[8 + lane];
        p[lane] = xyz + m[12 + lane];
        n[lane] = (vertex[4] * m[lane] + vertex[5] * m[4 + lane]) + vertex[6] * m[8 + lane];
    }
    memcpy(position, p, sizeof p);
    memcpy(normal, n, sizeof n);
}

float recomp_joint_sqrt(float x)
{
    const float r = 1.0f / sqrtf(x);
    const float cubed = ((x * r) * r) * r;
    return (r * 1.5f - cubed * 0.5f) * x;
}

/* Scales v[0..2] to unit length when its length is above zero, given the
   squared length the caller summed (in double, in the caller's order). */
static void unit(float v[3], double squared)
{
    if (squared == 0.0) return;
    const float length = recomp_joint_sqrt((float)squared);
    if (!(length > 0.0f)) return;
    const double k = 1.0 / (double)length;
    v[0] = (float)(k * v[0]);
    v[1] = (float)(k * v[1]);
    v[2] = (float)(k * v[2]);
}

void recomp_joint_frames(const float relative[16], const float data[40], float frames[64])
{
    for (unsigned axis = 0; axis < 3; ++axis) {
        float *f = frames + axis * 16u;
        const float *a = data + axis * 8u, *b = a + 4;
        memcpy(f, a, 4 * sizeof(float));
        recomp_joint_rotate(relative, b, f + 4);
        float *h = f + 8;
        h[0] = (float)((double)f[0] + f[4]);
        h[1] = (float)((double)f[5] + f[1]);
        h[2] = (float)((double)f[2] + f[6]);
        h[3] = (float)((double)f[7] + f[3]);
        unit(h, ((double)h[0] * h[0] + (double)h[2] * h[2]) + (double)h[1] * h[1]);
    }
    float *o = frames + 48;
    const float *points = data + 24;
    memcpy(o, points, 4 * sizeof(float));
    recomp_joint_transform(relative, points + 4, o + 4);
    recomp_joint_transform(relative, points + 8, o + 8);
    o[8] = (float)(((double)o[8] + points[12]) * 0.5);
    o[9] = (float)(((double)points[13] + o[9]) * 0.5);
    o[10] = (float)(((double)points[14] + o[10]) * 0.5);
}

void recomp_joint_weights(unsigned k, float weights[4])
{
    const double t = (double)(k & 15u) / 16.0;
    weights[0] = (float)((1.0 - t) * (1.0 - 2.0 * t));
    weights[1] = (float)(t * (2.0 * t - 1.0));
    weights[2] = (float)(4.0 * t * (1.0 - t));
    weights[3] = 0.0f;
    /* The table's own signs: -0.0 where its weight vanishes. */
    if (k == 0u) weights[1] = -0.0f;
    if (k == 8u) weights[0] = -0.0f;
}

/* a minus its part along b, as the game's 0x0017B080: unchanged when a.b
   or b.b is zero. */
static void remove_along(float a[4], const float b[4])
{
    const float along = (float)(((double)a[0] * b[0] + (double)b[2] * a[2]) + (double)b[1] * a[1]);
    const float length = (float)(((double)b[0] * b[0] + (double)b[1] * b[1]) + (double)b[2] * b[2]);
    if (along == 0.0f || length == 0.0f) return;
    const double t = (double)along / (double)length;
    const float ty = (float)(t * b[1]), tz = (float)(t * b[2]);
    a[0] = (float)((double)a[0] - t * b[0]);
    a[1] = (float)((double)a[1] - ty);
    a[2] = (float)((double)a[2] - tz);
}

static double squared(const float v[3])
{
    return ((double)v[0] * v[0] + (double)v[1] * v[1]) + (double)v[2] * v[2];
}

void recomp_joint_blend_frame(const float frames[64], const float weights[4], float rows[16])
{
    for (unsigned row = 0; row < 4; ++row) recomp_joint_rotate(frames + row * 16u, weights, rows + row * 4u);
    float *x = rows, *y = rows + 4, *z = rows + 8;
    unit(z, squared(z));
    remove_along(y, z);
    unit(y, squared(y));
    remove_along(x, z);
    remove_along(x, y);
    unit(x, squared(x));
}
