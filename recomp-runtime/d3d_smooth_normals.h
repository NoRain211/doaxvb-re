#ifndef RECOMP_D3D_SMOOTH_NORMALS_H
#define RECOMP_D3D_SMOOTH_NORMALS_H

#include "d3d_draw_model.h"
#include <vector>

// Returns a private vertex copy with area-weighted normals. Empty means an
// unsupported or invalid input; neither source buffer is modified. Subtracts
// first_vertex from each index when vertices points into a larger buffer.
std::vector<uint8_t> recomp_d3d_smooth_normals(
    const void *vertices, uint32_t count, uint32_t stride,
    const RecompD3dVertexLayout &layout,
    const uint16_t *indices, uint32_t index_count, uint32_t primitive,
    uint32_t first_vertex = 0);

#endif
