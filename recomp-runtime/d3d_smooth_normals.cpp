#include "d3d_smooth_normals.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace {
struct Vec3 {
    float x, y, z;
    Vec3 operator+(Vec3 b) const { return {x+b.x, y+b.y, z+b.z}; }
    Vec3 operator-(Vec3 b) const { return {x-b.x, y-b.y, z-b.z}; }
    Vec3 operator*(float s) const { return {x*s, y*s, z*s}; }
};
float dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
Vec3 unit(Vec3 v)
{
    const float n = dot(v, v);
    return n > 1e-20f && std::isfinite(n) ? v * (1.0f/std::sqrt(n)) : Vec3{};
}
constexpr float kWeldTolerance = 1e-4f;
struct Cell {
    int x, y, z;
    bool operator==(const Cell &b) const { return x==b.x && y==b.y && z==b.z; }
};
struct CellHash {
    size_t operator()(Cell c) const
    {
        return uint32_t(c.x)*73856093u ^ uint32_t(c.y)*19349663u ^ uint32_t(c.z)*83492791u;
    }
};
}

std::vector<uint8_t> recomp_d3d_smooth_normals(
    const void *vertices, uint32_t count, uint32_t stride,
    const RecompD3dVertexLayout &layout,
    const uint16_t *indices, uint32_t index_count, uint32_t primitive, uint32_t first_vertex)
{
    if (!vertices || !indices || !count || count > 65536u || stride < 12u ||
        layout.pretransformed || layout.position_offset > stride-12u ||
        layout.normal_offset > stride-12u || layout.blend_weight_count > 3u ||
        (layout.blend_weight_count && stride < 12u+4u*layout.blend_weight_count) ||
        index_count < 3u || primitive < 5u || primitive > 7u ||
        (primitive == 5u && index_count % 3u)) return {};

    const auto *bytes = static_cast<const uint8_t *>(vertices);
    std::vector<Vec3> positions(count), normals(count), sums(count);
    std::vector<bool> used(count);
    for (uint32_t i=0; i<index_count; ++i) {
        if (indices[i] < first_vertex || indices[i]-first_vertex >= count) return {};
        used[indices[i]-first_vertex] = true;
    }
    std::vector<uint32_t> welded(count), next(count, UINT32_MAX), heads(count, UINT32_MAX);
    const auto same_position = [&](uint32_t i, uint32_t j) {
        const Vec3 delta = positions[i]-positions[j];
        return dot(delta, delta)<=kWeldTolerance*kWeldTolerance &&
            (!layout.blend_weight_count || !std::memcmp(bytes+size_t(i)*stride+12u,
                bytes+size_t(j)*stride+12u, 4u*layout.blend_weight_count));
    };
    std::unordered_multimap<Cell, uint32_t, CellHash> grid;
    grid.reserve(count);
    for (uint32_t i=0; i<count; ++i) {
        if (!used[i]) continue;
        std::memcpy(&positions[i], bytes+size_t(i)*stride+layout.position_offset, 12u);
        std::memcpy(&normals[i], bytes+size_t(i)*stride+layout.normal_offset, 12u);
        const Vec3 p = positions[i];
        // Bound conversion to grid coordinates, including neighbor-cell offsets.
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
            std::fabs(p.x)>100000 || std::fabs(p.y)>100000 || std::fabs(p.z)>100000)
            return {};
        normals[i] = unit(normals[i]);
        const auto cell = [](Vec3 v) {
            constexpr double width = 2.0*double(kWeldTolerance);
            return Cell{int(std::floor(v.x/width)), int(std::floor(v.y/width)), int(std::floor(v.z/width))};
        };
        const Vec3 tolerance{kWeldTolerance, kWeldTolerance, kWeldTolerance};
        const Cell low = cell(p-tolerance), high = cell(p+tolerance);
        welded[i] = i;
        // Double-width cells need at most eight buckets for the tolerance box.
        for (int x=low.x; x<=high.x; ++x) for (int y=low.y; y<=high.y; ++y) for (int z=low.z; z<=high.z; ++z) {
            const auto range = grid.equal_range({x,y,z});
            for (auto it=range.first; it!=range.second; ++it)
                if (same_position(i, it->second) && it->second<welded[i])
                    welded[i] = it->second;
        }
        if (welded[i]==i) grid.emplace(cell(p), i);
        next[i] = heads[welded[i]];
        heads[welded[i]] = i;
    }
    std::vector<uint32_t> edges;
    edges.reserve(primitive==5u ? index_count : 3u*size_t(index_count-2u));
    const auto edge = [&](uint32_t a, uint32_t b) {
        a = welded[a]; b = welded[b];
        edges.push_back(a<b ? (a<<16u)|b : (b<<16u)|a);
    };
    const uint32_t triangles = primitive == 5u ? index_count/3u : index_count-2u;
    for (uint32_t t=0; t<triangles; ++t) {
        const uint32_t a = indices[primitive==5u ? t*3u : primitive==7u ? 0u : t]-first_vertex;
        const uint32_t b = indices[primitive==5u ? t*3u+1u : t+1u]-first_vertex;
        const uint32_t c = indices[primitive==5u ? t*3u+2u : t+2u]-first_vertex;
        Vec3 face = cross(positions[b]-positions[a], positions[c]-positions[a]);
        if (primitive==6u && (t&1u)) face = face * -1.0f;
        // Guest winding may face either way; retain the authored normal hemisphere.
        if (dot(face, normals[a]+normals[b]+normals[c]) < 0) face = face * -1.0f;
        const float area_squared = dot(face, face);
        if (!std::isfinite(area_squared) || area_squared<=1e-20f) continue;
        edge(a, b); edge(b, c); edge(c, a);
        sums[a] = sums[a]+face;
        sums[b] = sums[b]+face;
        sums[c] = sums[c]+face;
    }
    // Draws can contain only a patch of a smooth surface. Keep the authored
    // normals where the missing neighboring faces would bias the reconstruction.
    std::vector<bool> boundary(count);
    std::sort(edges.begin(), edges.end());
    for (size_t begin=0, end; begin<edges.size(); begin=end) {
        for (end=begin+1; end<edges.size() && edges[end]==edges[begin]; ++end) {}
        if (end-begin!=2u) {
            boundary[edges[begin]>>16u] = true;
            boundary[edges[begin]&65535u] = true;
        }
    }
    std::vector<uint8_t> result(bytes, bytes+size_t(count)*stride);
    for (uint32_t i=0; i<count; ++i) {
        if (!used[i] || boundary[welded[i]] || dot(normals[i], normals[i]) == 0) continue;
        Vec3 sum{};
        for (uint32_t j=heads[welded[i]]; j!=UINT32_MAX; j=next[j]) {
            if (same_position(i, j) && dot(normals[i], normals[j])>=0.5f)
                sum = sum+sums[j];
        }
        const Vec3 normal = unit(sum);
        if (dot(normal, normal)>0)
            std::memcpy(result.data()+size_t(i)*stride+layout.normal_offset, &normal, 12u);
    }
    return result;
}
