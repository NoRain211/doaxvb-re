#include "animation_pose.h"
#include "animation_rotation.h"

#include <math.h>
#include <string.h>

static void blend_direction(const float a[2], const float b[2], float weight,
    bool shoulder, float out[2])
{
    float sa, ca, azs, azc, sb, cb, bzs, bzc;
    float complement = 1.0f-weight;
    recomp_animation_sine_cosine(a[0], &sa, &ca);
    recomp_animation_sine_cosine(a[1], &azs, &azc);
    recomp_animation_sine_cosine(b[0], &sb, &cb);
    recomp_animation_sine_cosine(b[1], &bzs, &bzc);
    float vertical, horizontal, depth, length;
    if (shoulder) {
        float ay = -(azs*ca), az = azc*ca;
        float by = -(bzs*cb), bz = bzc*cb;
        vertical = (float)((double)complement*sa+(double)weight*sb);
        horizontal = (float)((double)complement*ay+(double)weight*by);
        depth = (float)((double)complement*az+(double)weight*bz);
        length = sqrtf((float)((double)depth*depth+(double)horizontal*horizontal));
        horizontal = -horizontal;
    } else {
        float first = (float)((double)ca*complement);
        float second = (float)((double)cb*weight);
        horizontal = (float)((double)bzs*second)+(float)((double)azs*first);
        double z = (double)bzc*second+(float)((double)azc*first);
        depth = (float)z;
        vertical = -((float)(-(double)sb*weight)+(float)(-(double)sa*complement));
        length = sqrtf((float)((double)horizontal*horizontal+z*depth));
    }
    out[0] = atan2f(vertical, length);
    out[1] = atan2f(horizontal, depth);
}

bool recomp_animation_pose_blend(const RecompAnimationGroup *groups, size_t count,
    const RecompAnimationPose *a, const RecompAnimationPose *b, float weight,
    RecompAnimationPose *output)
{
    static const unsigned widths[] = {3,3,2,2,2,1};
    if (!groups || !a || !b || !output || count > 60 ||
        !isfinite(weight) || weight < 0 || weight > 1) return false;
    RecompAnimationPose result = *output;
    uint32_t first = a->mask, second = b->mask;
    if (weight == 0) second &= ~first;
    if (weight == 1) first &= ~second;
    float complement = 1.0f-weight;
    for (size_t i = 0; i < count; ++i) {
        const RecompAnimationGroup *g = groups+i;
        if (g->kind >= sizeof widths/sizeof *widths || g->width != widths[g->kind] ||
            g->channel > 60-g->width) return false;
        float *out = result.channels+g->channel;
        const float *p = a->channels+g->channel, *q = b->channels+g->channel;
        bool from_a = (g->mask & first) != 0, from_b = (g->mask & second) != 0;
        for (unsigned j = 0; j < g->width; ++j)
            if ((from_a && (!isfinite(p[j]) || fabsf(p[j]) > 1000000)) ||
                (from_b && (!isfinite(q[j]) || fabsf(q[j]) > 1000000))) return false;
        if (!from_a && !from_b) continue;
        if (!from_a || !from_b) {
            memcpy(out, from_a ? p : q, g->width*sizeof(float));
        } else switch (g->kind) {
        case RECOMP_ANIMATION_TRANSLATION:
            for (unsigned j = 0; j < 3; ++j)
                out[j] = (float)((double)complement*p[j]+(double)weight*q[j]);
            break;
        case RECOMP_ANIMATION_EULER:
            recomp_animation_blend_euler(p, q, weight, out);
            break;
        case RECOMP_ANIMATION_DIRECTION:
        case RECOMP_ANIMATION_EYE_DIRECTION:
        case RECOMP_ANIMATION_SHOULDER_DIRECTION:
            if (g->kind == RECOMP_ANIMATION_EYE_DIRECTION && p[0] == q[0] && p[1] == q[1])
                memcpy(out, p, 2*sizeof(float));
            else blend_direction(p, q, weight, g->kind == RECOMP_ANIMATION_SHOULDER_DIRECTION, out);
            break;
        case RECOMP_ANIMATION_WRAPPED_ANGLE: {
            const float pi = 3.14159265358979323846f;
            double delta = (double)q[0]-p[0];
            if (delta <= -pi || delta >= 2.0f*pi) {
                float remainder = fmodf(fabsf((float)delta), 2.0f*pi);
                delta = delta < 0 ? 2.0f*pi-remainder : remainder;
            }
            if (delta >= pi) delta -= 2.0f*pi;
            out[0] = (float)((double)p[0]+delta*weight);
            break;
        }
        }
    }
    const float radians_per_turn_unit = 6.28318530717958647692f/65536.0f;
    const float turn_units_per_radian = 65536.0f/6.28318530717958647692f;
    for (unsigned i = 3; i < 6; ++i) {
        if (!isfinite(result.channels[i]) || fabsf(result.channels[i]) > 1000000) return false;
        int64_t units = (int64_t)((double)result.channels[i]*turn_units_per_radian);
        result.channels[i] = (float)((double)(uint16_t)units*radians_per_turn_unit);
    }
    for (unsigned i = 0; i < 60; ++i) if (!isfinite(result.channels[i])) return false;
    result.tick = result.flags = 0;
    *output = result;
    return true;
}
