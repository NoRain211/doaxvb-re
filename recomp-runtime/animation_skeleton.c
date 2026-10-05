#include "animation_skeleton.h"
#include "animation_rotation.h"

#include <math.h>
#include <string.h>

static const float half_pi = 1.57079632679489661923f;

static RecompBoneMatrix identity(void)
{
    RecompBoneMatrix m = {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    return m;
}

/* A seventh-order sine series on a folded quarter turn reproduces the
   original approximation. Libm sin/cos noticeably change the bone basis. */
static float quarter_sine(float turns)
{
    unsigned quadrant = (unsigned)turns;
    float u = turns - (float)quadrant;
    if (quadrant & 1u) u = 1.0f - u;
    float square = u * u;
    float value = -0.004681754135318688f; /* -(pi/2)^7 / 7! */
    value = value * square + 0.07969262624616703f;
    value = value * square - 0.6459640975062462f;
    value = value * square + 1.5707963267948966f;
    value *= u;
    return (quadrant & 2u) ? -value : value;
}

static void sine_cosine(float angle, float *sine, float *cosine)
{
    if (!isfinite(angle)) { *sine = *cosine = NAN; return; }
    float turns = fabsf(angle) * (2.0f / 3.14159265358979323846f);
    /* Reduction keeps conversion defined for finite but very large channels. */
    if (turns >= 16777216.0f) turns = fmodf(turns, 4.0f);
    *sine = quarter_sine(turns);
    if (signbit(angle)) *sine = -*sine;
    *cosine = quarter_sine(turns + 1.0f);
}

static RecompBoneMatrix multiply(RecompBoneMatrix a, RecompBoneMatrix b)
{
    RecompBoneMatrix out;
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c)
            out.m[r*4+c] = (float)(((double)a.m[r*4]*b.m[c] +
                (double)a.m[r*4+1]*b.m[4+c]) +
                (double)a.m[r*4+2]*b.m[8+c] + (double)a.m[r*4+3]*b.m[12+c]);
    return out;
}

static RecompBoneMatrix translate(RecompBoneMatrix m, const float xyz[3])
{
    for (unsigned c = 0; c < 4; ++c)
        m.m[12+c] = (float)(((double)xyz[0]*m.m[c] +
            (double)xyz[1]*m.m[4+c]) + (double)xyz[2]*m.m[8+c] + m.m[12+c]);
    return m;
}

static RecompBoneMatrix rotate(RecompBoneMatrix m, unsigned axis, float angle)
{
    RecompBoneMatrix rotation = identity();
    unsigned a = (axis+1)%3, b = (axis+2)%3;
    float s, c;
    sine_cosine(angle, &s, &c);
    rotation.m[a*4+a] = rotation.m[b*4+b] = c;
    rotation.m[a*4+b] = s;
    rotation.m[b*4+a] = -s;
    return multiply(rotation, m);
}

static RecompBoneMatrix euler(RecompBoneMatrix m, const float xyz[3])
{
    return rotate(rotate(rotate(m, 2, xyz[2]), 1, xyz[1]), 0, xyz[0]);
}

static double dot(const float a[3], const float b[3])
{
    return ((double)a[0]*b[0] + (double)a[1]*b[1]) + (double)a[2]*b[2];
}

static void cross(const float a[3], const float b[3], float out[3])
{
    for (unsigned i = 0; i < 3; ++i)
        out[i] = (float)((double)a[(i+1)%3]*b[(i+2)%3] -
            (double)a[(i+2)%3]*b[(i+1)%3]);
}

static float normalize(float v[3])
{
    float length = sqrtf((float)dot(v, v));
    if (length > 0.0f)
        for (unsigned i = 0; i < 3; ++i) v[i] /= length;
    return length;
}

static float clamp(float x, float low, float high)
{
    return x < low ? low : x > high ? high : x;
}

static RecompBoneMatrix swing(const float a[3], const float b[3])
{
    RecompBoneMatrix m = identity();
    double cosine = dot(a, b);
    float axis[3];
    cross(a, b, axis);
    if (1.0 + cosine <= 1e-6) return m;
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c)
            m.m[r*4+c] = (float)((r == c ? cosine : 0.0) +
                (double)axis[r]*axis[c]/(1.0 + cosine));
    for (unsigned i = 0; i < 3; ++i) {
        unsigned aindex = (i+1)%3, bindex = (i+2)%3;
        m.m[aindex*4+bindex] += axis[i];
        m.m[bindex*4+aindex] -= axis[i];
    }
    return m;
}

static RecompBoneMatrix blend(RecompBoneMatrix a, RecompBoneMatrix b, float weight)
{
    RecompBoneMatrix transpose = identity(), relative, rotation = identity();
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c) transpose.m[r*4+c] = a.m[c*4+r];
    relative = multiply(b, transpose);
    float axis[3] = {relative.m[6]-relative.m[9], relative.m[8]-relative.m[2],
        relative.m[1]-relative.m[4]};
    float length = normalize(axis);
    float cosine = (float)(((double)relative.m[10]+relative.m[5]+relative.m[0]-1)*0.5);
    {
        float s, c;
        sine_cosine((float)(atan2((double)length*0.5, cosine)*weight), &s, &c);
        for (unsigned r = 0; r < 3; ++r)
            for (unsigned col = 0; col < 3; ++col)
                rotation.m[r*4+col] = (float)((r == col ? c : 0) +
                    (1.0-c)*(double)axis[r]*axis[col]);
        for (unsigned i = 0; i < 3; ++i) {
            unsigned u = (i+1)%3, v = (i+2)%3;
            rotation.m[u*4+v] += s*axis[i];
            rotation.m[v*4+u] -= s*axis[i];
        }
    }
    RecompBoneMatrix out = multiply(rotation, a);
    for (unsigned i = 12; i < 15; ++i)
        out.m[i] = (float)((1.0-weight)*a.m[i] + (double)weight*b.m[i]);
    return out;
}

static RecompBoneMatrix joint(RecompBoneMatrix parent, const float offset[4],
    const float angles[3])
{
    return euler(translate(parent, offset), angles);
}

static void mixed_offset(const RecompSkeletonTables *t, unsigned index, float w, float out[3])
{
    float complement = 1.0f-w;
    for (unsigned i = 0; i < 3; ++i)
        out[i] = (float)((double)complement*t->offsets[index][i] + (double)w*t->alternate_offsets[index][i]);
}

static void derived_joints(RecompBoneMatrix b[32], bool extra_blend)
{
    static const unsigned links[4][3] = {{26,16,8}, {27,20,14}, {24,5,4}, {25,11,10}};
    for (unsigned i = 0; i < 4; ++i) {
        unsigned out = links[i][0], parent = links[i][1], child = links[i][2];
        b[out] = multiply(b[parent], swing(b[parent].m, b[child].m));
        memcpy(b[out].m+12, b[child].m+12, 4*sizeof(float));
    }
    b[30] = blend(b[8], b[26], 0.5f);
    b[31] = blend(b[14], b[27], 0.5f);
    b[4] = blend(b[4], b[24], 0.5f);
    b[10] = blend(b[10], b[25], 0.5f);
    b[24] = extra_blend ? blend(b[4], b[24], 0.5f) : b[4];
    b[25] = extra_blend ? blend(b[10], b[25], 0.5f) : b[10];
    b[28] = b[24];
    b[29] = b[25];
    memcpy(b[28].m+12, b[5].m+12, 4*sizeof(float));
    memcpy(b[29].m+12, b[11].m+12, 4*sizeof(float));
}

static bool solve_limb(const RecompSkeletonTables *t, const float *p,
    const RecompSkeletonTargets *s, unsigned index, RecompBoneMatrix b[32])
{
    const RecompLimbTable *d = &t->limbs[index];
    const RecompLimbLengths *lengths = &s->lengths[index];
    float position[3], angles[3], anchor_offset[3];
    for (unsigned i = 0; i < 3; ++i) position[i] = p[d->position_channels[i]];
    RecompBoneMatrix target = translate(b[15], position);
    /* Two stores are significant: the actor's altitude cancels algebraically,
       but not necessarily after float rounding. */
    float height = s->terrain_disabled ? s->position[1] : s->limb_height[index];
    target.m[13] = (float)((double)target.m[13] + height - s->position[1]);
    target.m[13] = (float)((double)target.m[13] + s->position[1] - s->ground_height);
    if (d->target_mask & s->target_mask)
        for (unsigned i = 0; i < 3; ++i)
            target.m[12+i] = (float)((double)s->target_weight*s->target_offset[i]+target.m[12+i]);
    float sa, ca, sz, cz;
    sine_cosine(p[d->pole_channels[0]], &sa, &ca);
    sine_cosine(p[d->pole_channels[1]], &sz, &cz);
    float pole[3] = {ca*sz, -sa, ca*cz}, world_pole[3];
    for (unsigned i = 0; i < 3; ++i)
        world_pole[i] = (float)((double)pole[0]*b[15].m[i] +
            (double)pole[1]*b[15].m[4+i] + (double)pole[2]*b[15].m[8+i]);
    float weight = d->upper_body ? s->upper_morph : s->lower_morph;
    RecompBoneMatrix anchor = b[d->parent];
    if (weight > 0) {
        anchor = b[2];
        if (d->upper_body) {
            mixed_offset(t, 23, weight, anchor_offset);
            anchor = joint(anchor, anchor_offset, p+44);
            mixed_offset(t, 0, weight, anchor_offset);
            anchor = joint(anchor, anchor_offset, p+6);
            mixed_offset(t, 20, weight, anchor_offset);
            anchor = translate(anchor, anchor_offset);
        }
        mixed_offset(t, d->proximal, weight, anchor_offset);
    } else memcpy(anchor_offset, t->offsets[d->proximal], sizeof anchor_offset);
    anchor = translate(anchor, anchor_offset);
    float forward[3], up[3], side[3];
    for (unsigned i = 0; i < 3; ++i) forward[i] = target.m[12+i]-anchor.m[12+i];
    float distance_squared = (float)dot(forward, forward);
    float distance = normalize(forward);
    if (distance == 0) return false;
    double projection = dot(world_pole, forward);
    for (unsigned i = 0; i < 3; ++i)
        up[i] = (float)((double)world_pole[i]-(double)projection*forward[i]);
    if (normalize(up) == 0) return false;
    cross(up, forward, side);
    RecompBoneMatrix basis = identity();
    memcpy(basis.m, side, sizeof side);
    memcpy(basis.m+4, up, sizeof up);
    memcpy(basis.m+8, forward, sizeof forward);
    RecompBoneMatrix nominal_anchor = translate(b[d->parent], t->offsets[d->proximal]);
    memcpy(basis.m+12, nominal_anchor.m+12, 3*sizeof(float));
    float complement = 1.0f-weight;
    float first = (float)((double)complement*lengths->proximal+weight*(double)lengths->alternate_proximal);
    float second = (float)((double)complement*lengths->distal+weight*(double)lengths->alternate_distal);
    float difference = (float)((double)complement*lengths->squared_difference+
        weight*(double)lengths->alternate_squared_difference);
    if (first <= 0 || second <= 0) return false;
    float alpha = half_pi, beta = half_pi;
    if (distance > 0.001f) {
        alpha = acosf(clamp((float)(((double)distance_squared+difference)/(2.0*distance*first)), -1, 1));
        beta = acosf(clamp((float)(((double)distance_squared-difference)/(2.0*distance*second)), -1, 1));
    }
    if (s->minimum_bend) { alpha = fmaxf(0.1f, alpha); beta = fmaxf(0.1f, beta); }
    if (!d->positive_bend) { alpha = -alpha; beta = -beta; }
    basis = rotate(basis, 1, alpha);
    for (unsigned i = 0; i < 3; ++i) angles[i] = d->proximal_quarters[i]*half_pi;
    b[d->proximal] = euler(basis, angles);
    /* Morph changes the solve angles, but translation uses the original lengths. */
    float along[3] = {0, 0, lengths->proximal};
    basis = rotate(translate(basis, along), 1, -beta-alpha);
    for (unsigned i = 0; i < 3; ++i) angles[i] = d->middle_quarters[i]*half_pi;
    b[d->middle] = euler(basis, angles);
    along[2] = lengths->distal;
    basis = translate(basis, along);
    memcpy(basis.m, b[15].m, 12*sizeof(float));
    for (unsigned i = 0; i < 3; ++i) angles[i] = p[d->rotation_channels[i]];
    b[d->end] = euler(basis, angles);
    if (d->tip != 255)
        b[d->tip] = rotate(translate(b[d->end], t->offsets[d->tip]), 0, p[d->tip_channel]);
    return true;
}

static void correct_terrain(const RecompSkeletonTables *t,
    const RecompSkeletonTargets *s, RecompBoneMatrix b[32])
{
    float height = s->neck_height-s->hip_height;
    float clearance = b[2].m[13]-s->hip_height;
    if (s->terrain_disabled || height == 0 || clearance >= 0.5f || clearance <= -0.1f)
        return;
    RecompBoneMatrix neck = translate(b[0], t->offsets[19]);
    float horizontal[3] = {neck.m[12]-b[2].m[12], 0, neck.m[14]-b[2].m[14]};
    float distance = normalize(horizontal);
    if (distance < 0.1f) return;
    float weight = 1.0f-2.0f*clearance;
    if (distance < 0.2f) weight *= (distance-0.1f)*10.0f;
    float slope[3] = {horizontal[0], height*clamp(weight, 0, 1), horizontal[2]};
    normalize(slope);
    RecompBoneMatrix correction = swing(horizontal, slope);
    /* Rotate about the hip in world space. */
    float pivot[3] = {-b[2].m[12], -b[2].m[13], -b[2].m[14]};
    correction = translate(correction, pivot);
    for (unsigned i = 0; i < 3; ++i) correction.m[12+i] += b[2].m[12+i];
    b[2] = multiply(b[2], correction);
    b[23] = multiply(b[23], correction);
    b[0] = multiply(b[0], correction);
}

static void rotation_angles(RecompBoneMatrix m, float xyz[3])
{
    float horizontal = sqrtf((float)((double)m.m[6]*m.m[6]+(double)m.m[10]*m.m[10]));
    if (horizontal > 1e-8f) {
        xyz[0] = atan2f(m.m[6], m.m[10]);
        xyz[1] = atan2f(-m.m[2], horizontal);
        xyz[2] = atan2f(m.m[1], m.m[0]);
    } else {
        xyz[0] = atan2f(m.m[2] > 0 ? -m.m[4] : m.m[4], m.m[5]);
        xyz[1] = m.m[2] > 0 ? -half_pi : half_pi;
        xyz[2] = 0;
    }
}

static void look_angles(const RecompSkeletonTables *t, const RecompSkeletonTargets *s,
    RecompBoneMatrix body, const float p[60], float neck[3], float head[3])
{
    memcpy(neck, p+47, 3*sizeof(float));
    memcpy(head, p+9, 3*sizeof(float));
    if (s->look_weight <= 0) return;
    float local[3], delta[3];
    for (unsigned i = 0; i < 3; ++i) {
        if (!isfinite(s->look_target[i])) return;
        delta[i] = s->look_target[i]-body.m[12+i];
    }
    /* The original rigid inverse transposes the approximate rotation; replacing
       this with a general inverse would change the look-at result. */
    for (unsigned i = 0; i < 3; ++i)
        local[i] = (float)(dot(delta, body.m+i*4)-t->offsets[19][i]-t->offsets[1][i]);
    local[0] *= 0.5f;
    local[1] *= 0.5f;
    local[2] = (fmaxf(local[2], 0.05f)+1.0f)*0.5f;
    float pitch = atan2f(-local[1], sqrtf((float)((double)local[0]*local[0]+(double)local[2]*local[2])));
    float yaw = atan2f(local[0], local[2]);
    pitch = clamp(pitch, -1.0471976f, 0.17453294f);
    yaw = clamp(yaw, -1.0471976f, 1.0471976f);
    float neck_target[3] = {pitch*0.3f, yaw*0.3f, 0};
    float head_target[3] = {pitch*0.7f, yaw*0.7f, 0};
    rotation_angles(blend(euler(identity(), neck), euler(identity(), neck_target), s->look_weight), neck);
    rotation_angles(blend(euler(identity(), head), euler(identity(), head_target), s->look_weight), head);
}

static bool finite_values(const float *values, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) if (!isfinite(values[i])) return false;
    return true;
}

void recomp_animation_sine_cosine(float angle, float *sine, float *cosine)
{
    sine_cosine(angle, sine, cosine);
}

void recomp_animation_blend_euler(const float a[3], const float b[3], float weight,
    float output[3])
{
    rotation_angles(blend(euler(identity(), a), euler(identity(), b), weight), output);
}

bool recomp_animation_solve_skeleton(const RecompSkeletonTables *t, const float p[60],
    const RecompSkeletonTargets *s, RecompBoneMatrix output[32])
{
    if (!t || !p || !s || !output) return false;
    for (unsigned i = 0; i < 60; ++i)
        if (!isfinite(p[i]) || fabsf(p[i]) > 1000000.0f) return false;
    if (!finite_values(s->position, 3) || !isfinite(s->heading) || fabsf(s->heading) > 1000000.0f ||
        !isfinite(s->ground_height) || !isfinite(s->hip_height) || !isfinite(s->neck_height) ||
        !finite_values(s->limb_height, 4) || !finite_values(s->target_offset, 3) ||
        !finite_values(s->eyes, 4) || !isfinite(s->target_weight) || !isfinite(s->look_weight) ||
        !isfinite(s->upper_morph) || !isfinite(s->lower_morph) ||
        s->upper_morph < 0 || s->upper_morph > 1 || s->lower_morph < 0 || s->lower_morph > 1 ||
        s->look_weight < 0 || s->look_weight > 1) return false;
    for (unsigned i = 0; i < 24; ++i)
        if (!finite_values(t->offsets[i], 3) || !finite_values(t->alternate_offsets[i], 3)) return false;
    for (unsigned i = 0; i < 4; ++i) {
        const RecompLimbTable *d = &t->limbs[i];
        const RecompLimbLengths *l = &s->lengths[i];
        if (!isfinite(l->proximal) || !isfinite(l->distal) || !isfinite(l->squared_difference) ||
            !isfinite(l->alternate_proximal) || !isfinite(l->alternate_distal) ||
            !isfinite(l->alternate_squared_difference) || fabsf(s->eyes[i]) > 1000000.0f)
            return false;
        if (d->end >= 32 || d->parent >= 32 || d->proximal >= 24 || d->middle >= 32 ||
            (d->tip != 255 && d->tip >= 24) || d->tip_channel >= 60) return false;
        for (unsigned k = 0; k < 3; ++k)
            if (d->position_channels[k] >= 60 || d->rotation_channels[k] >= 60) return false;
        if (d->pole_channels[0] >= 60 || d->pole_channels[1] >= 60) return false;
    }
    RecompBoneMatrix b[32];
    for (unsigned i = 0; i < 32; ++i) b[i] = identity();
    b[15] = rotate(translate(identity(), s->position), 1, s->heading);
    float hip[3] = {0, p[1], 0};
    b[2] = joint(b[15], hip, p+3);
    b[23] = joint(b[2], t->offsets[23], p+44);
    b[0] = joint(b[23], t->offsets[0], p+6);
    correct_terrain(t, s, b);
    b[20] = rotate(rotate(translate(b[0], t->offsets[20]), 1, p[51]), 2, p[50]);
    b[16] = rotate(rotate(translate(b[0], t->offsets[16]), 1, p[53]), 2, p[52]);
    float neck[3], head[3];
    look_angles(t, s, b[0], p, neck, head);
    b[19] = joint(b[0], t->offsets[19], neck);
    b[1] = joint(b[19], t->offsets[1], head);
    b[21] = rotate(rotate(translate(b[1], t->offsets[21]), 1, s->eyes[1]), 0, s->eyes[0]);
    b[17] = rotate(rotate(translate(b[1], t->offsets[17]), 1, s->eyes[3]), 0, s->eyes[2]);
    for (unsigned i = 0; i < 4; ++i) if (!solve_limb(t, p, s, i, b)) return false;
    derived_joints(b, s->derived_blend != 0);
    for (unsigned i = 0; i < 32; ++i)
        for (unsigned j = 0; j < 16; ++j) if (!isfinite(b[i].m[j])) return false;
    memcpy(output, b, sizeof b);
    return true;
}
