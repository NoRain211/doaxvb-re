#ifndef DOAXBV_D3D_MATRIX_H
#define DOAXBV_D3D_MATRIX_H

#include <cmath>
#include <utility>

/* Row-major 4x4 matrices used with row vectors (v' = v * M), the D3D convention.
   Layout matches HLSL row_major float4x4 and the draw command's float[16]. */
struct Float4 { float x, y, z, w; };
struct Float4x4 { float m[4][4]; };

inline Float4x4 operator*(const Float4x4 &a, const Float4x4 &b)
{
    Float4x4 result{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k)
        result.m[r][c] += a.m[r][k] * b.m[k][c];
    return result;
}

inline Float4 operator*(const Float4 &v, const Float4x4 &m)
{
    return {v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + v.w * m.m[3][0],
        v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + v.w * m.m[3][1],
        v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + v.w * m.m[3][2],
        v.x * m.m[0][3] + v.y * m.m[1][3] + v.z * m.m[2][3] + v.w * m.m[3][3]};
}

// Gauss-Jordan in double; false for a singular or non-finite matrix.
inline bool matrixInverse(const float matrix[16], float inverse[16])
{
    double rows[4][8]{};
    for (unsigned r = 0; r < 4; ++r) {
        for (unsigned c = 0; c < 4; ++c) {
            if (!std::isfinite(matrix[r*4+c])) return false;
            rows[r][c] = matrix[r*4+c];
        }
        rows[r][r+4] = 1;
    }
    for (unsigned c = 0; c < 4; ++c) {
        unsigned pivot = c;
        for (unsigned r = c+1; r < 4; ++r)
            if (std::fabs(rows[r][c]) > std::fabs(rows[pivot][c])) pivot = r;
        if (std::fabs(rows[pivot][c]) < 1e-12) return false;
        for (unsigned i = 0; i < 8; ++i) std::swap(rows[c][i], rows[pivot][i]);
        const double scale = rows[c][c];
        for (double &v : rows[c]) v /= scale;
        for (unsigned r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double factor = rows[r][c];
            for (unsigned i = 0; i < 8; ++i) rows[r][i] -= factor*rows[c][i];
        }
    }
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) {
        inverse[r*4+c] = static_cast<float>(rows[r][c+4]);
        if (!std::isfinite(inverse[r*4+c])) return false;
    }
    return true;
}

inline bool matrixInverse(const Float4x4 &matrix, Float4x4 &inverse)
{
    return matrixInverse(&matrix.m[0][0], &inverse.m[0][0]);
}

#endif
