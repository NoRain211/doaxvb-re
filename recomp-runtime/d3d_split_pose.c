#include "d3d_split_pose.h"
#include "animation_seam.h"
#include "animation_rotation.h"
#include "d3d_pose_replay.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

double recomp_split_rate(const char *text)
{
    if (!text || !*text) return 0;
    char *end;
    double rate = strtod(text, &end);
    while (isspace((unsigned char)*end)) ++end;
    if (*end || !isfinite(rate) || rate < 60 || rate > 1000) return 0;
    return rate;
}

bool recomp_split_requested(const char *text)
{
    return (text && strcmp(text, "auto") == 0) || recomp_split_rate(text) != 0;
}

double recomp_split_display_rate(const char *text, double display_hz, bool vsync,
    const char **note)
{
    *note = NULL;
    const bool known = isfinite(display_hz) && display_hz >= 30 && display_hz <= 1000;
    if (text && strcmp(text, "auto") == 0) {
        if (known && display_hz >= 60) return display_hz;
        *note = known ? "display refresh is below 60 Hz; split rate off" :
            "display refresh unknown; split rate off";
        return 0;
    }
    double rate = recomp_split_rate(text);
    if (!rate || !known) return rate;
    // A nominal rate such as 120 on a 119.88 Hz mode would drift a frame every
    // few seconds; within 1% the display's exact rate is what was meant.
    if (fabs(rate-display_hz) <= display_hz*0.01) return display_hz;
    if (vsync && rate > display_hz) {
        *note = "rate exceeds the display refresh; using the display refresh";
        return display_hz;
    }
    *note = "rate differs from the display refresh; motion will judder unless the display is variable refresh";
    return rate;
}

/* The full attract cycle scored >= 20 at every shot change and <= 4 for
   continuous motion; floors keep motion starting from rest below the ratio. */
bool recomp_split_discontinuity(float previous, float step, float floor)
{
    return !(step <= 8.0f*(previous+floor));
}

static double degrees_between(const double a[3], const double b[3])
{
    double dot = a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
    double length = sqrt((a[0]*a[0]+a[1]*a[1]+a[2]*a[2])*(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]));
    if (!(length > 0)) return 0;
    return acos(fmax(-1.0, fmin(1.0, dot/length)))*(180.0/3.14159265358979323846);
}

void recomp_split_camera_motion(const RecompVisualCamera pair[2], float motion[2])
{
    double eye = 0, target = 0, distance = 0, view[2][3];
    for (unsigned i = 0; i < 3; ++i) {
        eye += ((double)pair[1].eye[i]-pair[0].eye[i])*((double)pair[1].eye[i]-pair[0].eye[i]);
        target += ((double)pair[1].target[i]-pair[0].target[i])*((double)pair[1].target[i]-pair[0].target[i]);
        for (unsigned j = 0; j < 2; ++j) view[j][i] = (double)pair[j].target[i]-pair[j].eye[i];
        distance += view[1][i]*view[1][i];
    }
    motion[0] = (float)(sqrt(fmax(eye, target))/fmax(sqrt(distance), 1e-6));
    motion[1] = (float)degrees_between(view[0], view[1]);
}

void recomp_split_bone_motion(const RecompBoneMatrix *from, const RecompBoneMatrix *to, float motion[2])
{
    double distance = 0, angle = 0;
    for (unsigned i = 0; i < 3; ++i) {
        double d = (double)to->m[12+i]-from->m[12+i];
        distance += d*d;
        double a[3] = {from->m[i*4], from->m[i*4+1], from->m[i*4+2]};
        double b[3] = {to->m[i*4], to->m[i*4+1], to->m[i*4+2]};
        angle = fmax(angle, degrees_between(a, b));
    }
    motion[0] = (float)sqrt(distance);
    motion[1] = (float)angle;
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

static bool rotation_quaternion(const float m[16], double scale[3], double q[4])
{
    for (unsigned i = 0; i < 16; ++i) if (!isfinite(m[i])) return false;
    double r[3][3];
    for (unsigned i = 0; i < 3; ++i) {
        scale[i] = sqrt((double)m[i*4]*m[i*4]+(double)m[i*4+1]*m[i*4+1]+(double)m[i*4+2]*m[i*4+2]);
        if (!(scale[i] > 1e-12) || !isfinite(scale[i]) || !isfinite(m[12+i])) return false;
        for (unsigned j = 0; j < 3; ++j) r[i][j] = m[i*4+j]/scale[i];
    }
    for (unsigned i = 0; i < 3; ++i) for (unsigned j = i+1; j < 3; ++j)
        if (fabs(r[i][0]*r[j][0]+r[i][1]*r[j][1]+r[i][2]*r[j][2]) > 1e-3) return false;
    double det = r[0][0]*(r[1][1]*r[2][2]-r[1][2]*r[2][1])-r[0][1]*(r[1][0]*r[2][2]-r[1][2]*r[2][0])+
        r[0][2]*(r[1][0]*r[2][1]-r[1][1]*r[2][0]);
    if (det < 0.999 || det > 1.001) return false;
    double trace = r[0][0]+r[1][1]+r[2][2];
    if (trace > 0) {
        double s = sqrt(trace+1)*2;
        q[0] = s/4; q[1] = (r[2][1]-r[1][2])/s; q[2] = (r[0][2]-r[2][0])/s; q[3] = (r[1][0]-r[0][1])/s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        double s = sqrt(1+r[0][0]-r[1][1]-r[2][2])*2;
        q[0] = (r[2][1]-r[1][2])/s; q[1] = s/4; q[2] = (r[0][1]+r[1][0])/s; q[3] = (r[0][2]+r[2][0])/s;
    } else if (r[1][1] > r[2][2]) {
        double s = sqrt(1+r[1][1]-r[0][0]-r[2][2])*2;
        q[0] = (r[0][2]-r[2][0])/s; q[1] = (r[0][1]+r[1][0])/s; q[2] = s/4; q[3] = (r[1][2]+r[2][1])/s;
    } else {
        double s = sqrt(1+r[2][2]-r[0][0]-r[1][1])*2;
        q[0] = (r[1][0]-r[0][1])/s; q[1] = (r[0][2]+r[2][0])/s; q[2] = (r[1][2]+r[2][1])/s; q[3] = s/4;
    }
    return true;
}

bool recomp_split_rigid_matrix(const float from[16], const float to[16], float fraction, float output[16])
{
    double scale[2][3], q[2][4];
    if (!isfinite(fraction) || !rotation_quaternion(from, scale[0], q[0]) ||
        !rotation_quaternion(to, scale[1], q[1])) return false;
    double dot = q[0][0]*q[1][0]+q[0][1]*q[1][1]+q[0][2]*q[1][2]+q[0][3]*q[1][3];
    double sign = dot < 0 ? -1 : 1, a = 1-fraction, b = fraction;
    dot = fabs(dot);
    if (dot < 0.9995) {
        double angle = acos(fmin(dot, 1.0)), s = sin(angle);
        a = sin((1-fraction)*angle)/s; b = sin(fraction*angle)/s;
    }
    double r[4], length = 0;
    for (unsigned i = 0; i < 4; ++i) { r[i] = a*q[0][i]+b*sign*q[1][i]; length += r[i]*r[i]; }
    length = sqrt(length);
    double w = r[0]/length, x = r[1]/length, y = r[2]/length, z = r[3]/length;
    const double m[3][3] = {
        {1-2*(y*y+z*z), 2*(x*y-w*z), 2*(x*z+w*y)},
        {2*(x*y+w*z), 1-2*(x*x+z*z), 2*(y*z-w*x)},
        {2*(x*z-w*y), 2*(y*z+w*x), 1-2*(x*x+y*y)},
    };
    for (unsigned i = 0; i < 3; ++i) {
        double s = scale[0][i]+(scale[1][i]-scale[0][i])*fraction;
        for (unsigned j = 0; j < 3; ++j) output[i*4+j] = (float)(m[i][j]*s);
        output[i*4+3] = 0;
        output[12+i] = (float)(from[12+i]+((double)to[12+i]-from[12+i])*fraction);
    }
    output[15] = 1;
    return true;
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
    if (split->rigid && !recomp_split_rigid_matrix(split->rigid_worlds[0], split->rigid_worlds[1],
        fraction, pose.palettes[0][0])) return false;
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
