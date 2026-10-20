#include "d3d_smooth_normals.h"
#include <cmath>
#include <cstdio>
#include <cstring>

int main()
{
    struct Vertex { float position[3], normal[3], uv[2]; };
    Vertex v[] = {
        {{0,0,0}, {0,0,1}, {0,0}},
        {{1,0,0}, {0,0,1}, {1,0}},
        {{0,1,0}, {0,0,1}, {0,1}},
        {{-1,0,1}, {0,0,1}, {0,0}},
        {{0,-2,1}, {0,0,1}, {0,1}},
        {{-0.000002f,0,0}, {0.70710678f,0,0.70710678f}, {1,1}}
    };
    const uint16_t indices[] = {0,1,2,0,2,3,5,3,4,5,4,1};
    RecompD3dVertexLayout layout{};
    layout.normal_offset = 12;
    auto smooth = [&] {
        return recomp_d3d_smooth_normals(v, 6, sizeof(Vertex), layout, indices, 12, 5);
    };
    auto normal = [](const std::vector<uint8_t> &bytes, unsigned i, unsigned axis) {
        float value;
        std::memcpy(&value, bytes.data()+i*sizeof(Vertex)+12+axis*4, 4);
        return value;
    };
    const auto result = smooth();
    if (result.size() != sizeof v) return 1;
    // Unequal face areas, a UV split, and opposite sides of a grid boundary.
    for (unsigned i : {0u, 5u}) {
        if (std::fabs(normal(result, i, 0)-3/7.0f) > 1e-5f ||
            std::fabs(normal(result, i, 2)-6/7.0f) > 1e-5f) return 2;
    }
    for (unsigned i=0; i<6; ++i) {
        if (std::memcmp(result.data()+i*sizeof(Vertex), v[i].position, 12) ||
            std::memcmp(result.data()+i*sizeof(Vertex)+24, v[i].uv, 8)) return 3;
    }
    if (v[0].normal[0] != 0 || v[0].normal[2] != 1) return 4;
    uint16_t based[12];
    for (unsigned i=0; i<12; ++i) based[i]=indices[i]+37;
    const auto slice = recomp_d3d_smooth_normals(v, 6, sizeof(Vertex), layout, based, 12, 5, 37);
    if (slice!=result) return 12;
    // Authored normals 90 degrees apart must keep their separate faces.
    v[5].normal[0]=1; v[5].normal[2]=0;
    const auto hard = smooth();
    if (std::fabs(normal(hard, 0, 0)-1/std::sqrt(5.0f))>1e-5f ||
        std::fabs(normal(hard, 5, 0)-2/std::sqrt(24.0f))>1e-5f) return 5;
    // A CPU-skinned position update must change the freshly computed normal.
    v[1].position[2]=1;
    const auto moved = smooth();
    if (std::fabs(normal(moved, 0, 0))>1e-5f) return 6;
    const uint16_t bad[] = {0,1,6};
    if (!recomp_d3d_smooth_normals(v, 6, sizeof(Vertex), layout, bad, 3, 5).empty()) return 7;
    const uint16_t degenerate[] = {0,0,0};
    const auto unchanged = recomp_d3d_smooth_normals(v, 6, sizeof(Vertex), layout, degenerate, 3, 5);
    if (unchanged.size()!=sizeof v || std::memcmp(unchanged.data(), v, sizeof v)) return 8;
    const Vertex quad[] = {
        {{0,0,0},{0,0,1},{}}, {{1,0,0},{0,0,1},{}},
        {{0,1,0},{0,0,1},{}}, {{1,1,0},{0,0,1},{}}
    };
    for (uint32_t primitive : {6u, 7u}) {
        const uint16_t strip[] = {0,1,2,3}, fan[] = {0,1,3,2};
        const auto flat = recomp_d3d_smooth_normals(quad, 4, sizeof(Vertex), layout,
            primitive==6u ? strip : fan, 4, primitive);
        if (flat.size()!=sizeof quad || std::memcmp(flat.data(), quad, sizeof quad)) return 9;
    }
    struct Skinned { float position[3], weight, normal[3]; };
    Skinned skinned[6];
    for (unsigned i=0; i<6; ++i) {
        std::memcpy(skinned[i].position, v[i].position, 12);
        skinned[i].weight = i==5 ? 1.0f : 0.0f;
        skinned[i].normal[0]=0; skinned[i].normal[1]=0; skinned[i].normal[2]=1;
    }
    layout.normal_offset=16; layout.blend_weight_count=1;
    const auto separate = recomp_d3d_smooth_normals(skinned, 6, sizeof(Skinned), layout, indices, 12, 5);
    Skinned first;
    std::memcpy(&first, separate.data(), sizeof first);
    if (first.weight!=0 || first.normal[0]!=0 || first.normal[2]!=1) return 10;
    layout.normal_offset=12; layout.blend_weight_count=0;
    const auto patch = recomp_d3d_smooth_normals(v, 6, sizeof(Vertex), layout, indices, 6, 5);
    if (std::memcmp(patch.data(), v, sizeof v)) return 11;
    std::puts("PASS smooth normals: weighted seam, hard edge, animation, invalid and degenerate triangles");
    return 0;
}
