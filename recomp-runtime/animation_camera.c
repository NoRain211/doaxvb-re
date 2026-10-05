#include "animation_camera.h"
#include "animation_rotation.h"
#include <math.h>
#include <string.h>

static bool normalize(float vector[3])
{
    float squared = (float)(((double)vector[2]*vector[2]+(double)vector[1]*vector[1])+
        (double)vector[0]*vector[0]);
    if (!isfinite(squared) || squared <= 0) return false;
    float inverse = 1.0f/sqrtf(squared);
    float correction = squared*inverse;
    correction *= inverse; correction *= inverse; correction *= .5f;
    float length = (inverse*1.5f-correction)*squared;
    for (unsigned i = 0; i < 3; ++i) vector[i] = (float)(vector[i]*(1.0/length));
    return true;
}

/* The steep-camera branch quantizes an approximate arctangent to 16-bit turns. */
static float heading_angle(float ratio)
{
    float reduced = fabsf(ratio) > 1 ? 1.0f/ratio : ratio;
    float square = reduced*reduced;
    float value = (1.0f/(1.2797564268112183f+square))*-0.09164611995220184f;
    value += 2.1972169876098633f+square;
    value = (1.0f/value)*-1.395694613456726f;
    value += 6.819306373596191f+square;
    value = (1.0f/value)*-94.39392852783203f;
    value += 28.2052059173584f+square;
    value = (reduced*12.888382911682129f)*(1.0f/value);
    return fabsf(ratio) > 1 ? copysignf(1.5707963267948966f, ratio)-value : value;
}

bool recomp_animation_camera(const RecompVisualCamera *input, float view[16], float projection[16])
{
    if (!input || !view || !projection) return false;
    float values[sizeof *input/sizeof(float)];
    memcpy(values, input, sizeof values);
    for (unsigned i = 0; i < sizeof *input/sizeof(float); ++i) if (!isfinite(values[i])) return false;
    if (input->fov <= 0 || input->fov >= 3.14159f || input->near_z <= 0 ||
        input->far_z <= input->near_z || input->aspect <= 0) return false;
    double eye[3] = {input->eye[0], input->eye[1], -input->eye[2]};
    float forward[3] = {input->target[0]-input->eye[0], input->target[1]-input->eye[1],
        input->eye[2]-input->target[2]};
    if (!normalize(forward)) return false;
    float right[3] = {forward[2], 0, -forward[0]};
    if ((double)forward[1]*forward[1] < .5625) {
        if (!normalize(right)) return false;
    } else {
        float dx = input->eye[0]-input->target[0];
        float dz = input->eye[2]-input->target[2];
        float angle = dx == 0 && dz == 0 ? 0 : heading_angle(dx/dz);
        if (signbit(dz)) angle += copysignf(3.14159265358979323846f, dx);
        float quantized = (float)((int)(-(double)angle*10430.3779296875f)*
            (double)9.58738019107841e-5f);
        float sine, cosine;
        recomp_animation_sine_cosine(quantized, &sine, &cosine);
        right[0] = cosine; right[1] = 0; right[2] = -sine;
    }
    double up[3] = {(double)forward[1]*right[2], (double)forward[2]*right[0]-(double)forward[0]*right[2],
        -(double)forward[1]*right[0]};
    float sine = 0, cosine = 1;
    if (input->roll != 0) recomp_animation_sine_cosine(-input->roll, &sine, &cosine);
    float v[16] = {0}, p[16] = {0};
    for (unsigned row = 0; row < 3; ++row) {
        v[row*4] = (float)(right[row]*cosine-up[row]*sine);
        v[row*4+1] = (float)(right[row]*sine+up[row]*cosine);
        v[row*4+2] = (float)forward[row];
    }
    for (unsigned column = 0; column < 3; ++column)
        v[12+column] = (float)-(eye[0]*v[column]+eye[1]*v[4+column]+eye[2]*v[8+column]);
    v[15] = 1;
    for (unsigned column = 0; column < 4; ++column) v[8+column] = -v[8+column];
    double cotangent = 1.0/tan(input->fov*0.5);
    float depth = input->far_z/(input->far_z-input->near_z);
    p[0] = (float)(cotangent/input->aspect*input->scale_x);
    p[5] = (float)(cotangent*input->scale_y);
    p[8] = input->bias_x; p[9] = -input->bias_y;
    p[10] = depth*input->scale_z; p[11] = 1; p[14] = -depth*input->near_z;
    memcpy(view, v, sizeof v); memcpy(projection, p, sizeof p);
    return true;
}

bool recomp_animation_camera_sample(const RecompVisualCamera inputs[2], float fraction,
    float view[16], float projection[16])
{
    if (!inputs || !isfinite(fraction) || fraction < 0 || fraction > 1) return false;
    RecompVisualCamera sample;
    float a[sizeof sample/sizeof(float)], b[sizeof sample/sizeof(float)], out[sizeof sample/sizeof(float)];
    memcpy(a, inputs, sizeof a); memcpy(b, inputs+1, sizeof b);
    for (unsigned i = 0; i < sizeof sample/sizeof(float); ++i)
        out[i] = (float)(a[i]+((double)b[i]-a[i])*fraction);
    memcpy(&sample, out, sizeof sample);
    double angle = remainder((double)inputs[1].roll-inputs[0].roll, 6.28318530717958647692);
    sample.roll = (float)(inputs[0].roll+angle*fraction);
    return recomp_animation_camera(&sample, view, projection);
}
