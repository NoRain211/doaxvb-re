#include "animation_seam.h"

#include <math.h>
#include <string.h>

static bool finite_values(const float *values, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) if (!isfinite(values[i])) return false;
    return true;
}

static void transform(const float vector[3], const float matrix[16], bool point, float out[4])
{
    for (unsigned i = 0; i < 4; ++i) {
        float value = vector[0]*matrix[i]+vector[1]*matrix[4+i];
        value += vector[2]*matrix[8+i];
        out[i] = point ? value+matrix[12+i] : value;
    }
}

bool recomp_animation_seam_relative(const RecompBoneMatrix *source,
    const RecompBoneMatrix *destination, RecompBoneMatrix *output)
{
    if (!source || !destination || !output || !finite_values(source->m, 16) ||
        !finite_values(destination->m, 16)) return false;
    const float *m = destination->m;
    if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1) return false;
    double a=m[0], b=m[1], c=m[2], d=m[4], e=m[5], f=m[6], g=m[8], h=m[9], i=m[10];
    double determinant = a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
    if (fabs(determinant) < 1e-12) return false;
    double inverse[16] = {
        (e*i-f*h)/determinant, (c*h-b*i)/determinant, (b*f-c*e)/determinant, 0,
        (f*g-d*i)/determinant, (a*i-c*g)/determinant, (c*d-a*f)/determinant, 0,
        (d*h-e*g)/determinant, (b*g-a*h)/determinant, (a*e-b*d)/determinant, 0,
        0,0,0,1
    };
    for (unsigned column = 0; column < 3; ++column)
        inverse[12+column] = -(m[12]*inverse[column]+m[13]*inverse[4+column]+m[14]*inverse[8+column]);
    RecompBoneMatrix result;
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column) {
            double sum = 0;
            for (unsigned k = 0; k < 4; ++k) sum += source->m[row*4+k]*inverse[k*4+column];
            result.m[row*4+column] = (float)sum;
        }
    if (!finite_values(result.m, 16)) return false;
    *output = result;
    return true;
}

/* Preserve the scalar/SIMD refinement used by the ordinary seam writer. */
static bool normalize(float vector[4])
{
    float squared = (float)((double)vector[0]*vector[0]+(double)vector[2]*vector[2]+
        (double)vector[1]*vector[1]);
    if (!isfinite(squared)) return false;
    if (squared == 0) return true;
    float inverse = 1.0f/sqrtf(squared);
    float correction = squared*inverse;
    correction *= inverse;
    correction *= inverse;
    correction *= 0.5f;
    float length = (inverse*1.5f-correction)*squared;
    if (!isfinite(length) || length <= 0) return false;
    for (unsigned i = 0; i < 3; ++i) vector[i] = (float)((double)vector[i]/length);
    return true;
}

static void remove_projection(float vector[4], const float axis[4])
{
    double dot = (double)vector[0]*axis[0]+(double)vector[2]*axis[2]+(double)vector[1]*axis[1];
    double squared = (double)axis[0]*axis[0]+(double)axis[2]*axis[2]+(double)axis[1]*axis[1];
    if (dot == 0 || squared == 0) return;
    double factor = dot/squared;
    for (unsigned i = 0; i < 4; ++i) vector[i] = (float)(vector[i]-factor*axis[i]);
}

bool recomp_animation_seam_basis(const float reference[10][4],
    const RecompBoneMatrix *relative, RecompSeamBasis *output)
{
    if (!reference || !relative || !output || !finite_values(&reference[0][0], 40) ||
        !finite_values(relative->m, 16)) return false;
    RecompSeamBasis basis;
    for (unsigned axis = 0; axis < 3; ++axis) {
        memcpy(basis.rows[axis][0], reference[axis*2], 16);
        transform(reference[axis*2+1], relative->m, false, basis.rows[axis][1]);
        for (unsigned i = 0; i < 4; ++i)
            basis.rows[axis][2][i] = basis.rows[axis][0][i]+basis.rows[axis][1][i];
        if (!normalize(basis.rows[axis][2])) return false;
    }
    memcpy(basis.rows[3][0], reference[6], 16);
    transform(reference[7], relative->m, true, basis.rows[3][1]);
    transform(reference[8], relative->m, true, basis.rows[3][2]);
    /* This is the rig's center control, independent of presentation time. */
    for (unsigned i = 0; i < 3; ++i)
        basis.rows[3][2][i] = (basis.rows[3][2][i]+reference[9][i])*0.5f;
    if (!finite_values(&basis.rows[0][0][0], 48)) return false;
    *output = basis;
    return true;
}

bool recomp_animation_seam_matrix(const RecompSeamBasis *basis,
    const float coefficients[4], RecompBoneMatrix *output)
{
    if (!basis || !coefficients || !output || !finite_values(coefficients, 4)) return false;
    RecompBoneMatrix matrix;
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column) {
            float value = coefficients[0]*basis->rows[row][0][column]+coefficients[1]*basis->rows[row][1][column];
            matrix.m[row*4+column] = value+coefficients[2]*basis->rows[row][2][column];
        }
    if (!normalize(matrix.m+8)) return false;
    remove_projection(matrix.m+4, matrix.m+8);
    if (!normalize(matrix.m+4)) return false;
    remove_projection(matrix.m, matrix.m+8);
    remove_projection(matrix.m, matrix.m+4);
    if (!normalize(matrix.m) || !finite_values(matrix.m, 16)) return false;
    *output = matrix;
    return true;
}

void recomp_animation_seam_vertex(const RecompBoneMatrix *matrix,
    const float position[3], const float normal[3], float output[6])
{
    float p[4], n[4];
    transform(position, matrix->m, true, p);
    transform(normal, matrix->m, false, n);
    memcpy(output, p, 12);
    memcpy(output+3, n, 12);
}
