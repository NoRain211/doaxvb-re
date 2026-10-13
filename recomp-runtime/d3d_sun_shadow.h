#ifndef DOAXBV_D3D_SUN_SHADOW_H
#define DOAXBV_D3D_SUN_SHADOW_H

// Internal D3D11 experiment; all GPU objects are created only on explicit opt-in.
namespace {

// 4096 texels over 64 units: ~0.016-unit texels, finer than the old 2048/48.
constexpr unsigned kSunMapSize = 4096;
constexpr float kSunBox = 64, kSunAhead = 20, kSunDepth = 120;

struct SunShadow {
    bool enabled = false, valid = false, casting = false;
    unsigned epoch = 0, camera_epoch = 0, triangles = 0, casters = 0, game_shadows = 0;
    // Game shadow opacity (texture-factor alpha) seen last frame; 0 when the game drew none.
    float game_alpha = 0, seen_alpha = 0;
    float camera[16]{}, inverse[16]{}, projection[16]{}, direction[3]{};
    ID3D11Texture2D *texture = nullptr;
    ID3D11DepthStencilView *depth = nullptr;
    ID3D11ShaderResourceView *view = nullptr;
    ID3D11Buffer *constants = nullptr;
    ID3D11SamplerState *sampler = nullptr;
    ID3D11RasterizerState *rasterizer = nullptr;
};

bool sunShadowRequested()
{
    const char *value = std::getenv("RECOMP_D3D_SUN_SHADOW");
    return value && std::strcmp(value, "1") == 0;
}

bool sunNormalize(float v[3])
{
    const float length = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (!std::isfinite(length) || length < 1e-6f) return false;
    for (unsigned i = 0; i < 3; ++i) v[i] /= length;
    return true;
}

// Picks the draw's directional light nearest the background key light (adapter's sun);
// direction receives light travel for the map. Captured XYZ is world-space toward-light.
// shortcut: matches by direction (within 25 deg), not slot tag; add slot provenance if rigs collide.
int sunDirection(const RecompD3dPresenterDrawCommand &draw, float direction[3])
{
    const RecompD3dDirectionalLighting &light = draw.directional;
    if (draw.sun[3] != 1 || !light.enabled || light.count > 8) return -1;
    int selected = -1;
    float best = 0.906f; // cos 25 degrees
    for (unsigned i = 0; i < light.count; ++i) {
        if (light.positions[i][3] != 0) continue;
        float toward[3] = {light.directions[i][0], light.directions[i][1], light.directions[i][2]};
        if (!sunNormalize(toward)) continue;
        const float match = toward[0]*draw.sun[0] + toward[1]*draw.sun[1] + toward[2]*draw.sun[2];
        if (match > best) { best = match; selected = static_cast<int>(i); }
    }
    if (selected < 0) return -1;
    for (unsigned c = 0; c < 3; ++c) direction[c] = -draw.sun[c];
    if (const char *value = std::getenv("RECOMP_D3D_SUN_DIR")) {
        float override_direction[3];
        char trailing;
        if (std::sscanf(value, " %f , %f , %f %c", &override_direction[0],
                &override_direction[1], &override_direction[2], &trailing) == 3 &&
            sunNormalize(override_direction))
            std::memcpy(direction, override_direction, sizeof override_direction);
    }
    // A key light at or below the horizon (one demo rig) casts nothing useful; keep the game's shadow.
    if (direction[1] > -0.05f) return -1;
    return selected;
}

// The game's projected shadow (actors and ball), darkening pass only. That pass also
// zeroes the stencil it read, so the caller keeps it with color writes off.
bool sunGameShadow(const RecompD3dPresenterDrawCommand &draw)
{
    const auto &d = draw.depth;
    return draw.fvf == 0x112u && !draw.has_texture && draw.use_texture_factor && !draw.target.offscreen &&
        !draw.directional.enabled && (draw.texture_factor & 0xffffffu) == 0 && (draw.texture_factor >> 24) != 0 &&
        d.stencil_enable && d.stencil_func == RECOMP_D3D_COMPARE_EQUAL && d.stencil_ref == 1 &&
        (d.stencil_read_mask & 3u) == 3u && d.stencil_pass == RECOMP_D3D_STENCIL_ZERO &&
        d.stencil_fail == RECOMP_D3D_STENCIL_KEEP && d.stencil_zfail == RECOMP_D3D_STENCIL_KEEP &&
        d.depth_test_enable && !d.depth_write_enable && draw.blend.blend_enable && draw.blend.color_write_mask &&
        draw.blend.src_factor == RECOMP_D3D_BLEND_SRC_ALPHA && draw.blend.dst_factor == RECOMP_D3D_BLEND_INV_SRC_ALPHA;
}

bool sunInverse(const float matrix[16], float inverse[16])
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

bool sunSceneDraw(const RecompD3dPresenterDrawCommand &draw, float &extent)
{
    RecompD3dVertexLayout layout;
    if (draw.target.offscreen || draw.target.no_depth || !draw.has_transform || draw.program_count ||
        !draw.vertex_bytes || !draw.index_bytes || !draw.vertex_count || !draw.triangle_count ||
        !draw.depth.depth_test_enable || !draw.depth.depth_write_enable ||
        !recomp_d3d_fvf_layout(draw.fvf, &layout) || layout.pretransformed ||
        layout.normal_offset == RECOMP_D3D_FVF_ABSENT || layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT ||
        layout.stride > draw.vertex_stride || draw.blend_weight_count > 3 ||
        (draw.blend_weight_count && draw.blend_weight_count != layout.blend_weight_count)) return false;
    float minimum[3] = {INFINITY, INFINITY, INFINITY}, maximum[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (unsigned v = 0; v < draw.vertex_count; ++v) {
        float vertex[6]{};
        std::memcpy(vertex, static_cast<const uint8_t *>(draw.vertex_bytes) + size_t(v)*draw.vertex_stride,
            (3+draw.blend_weight_count)*sizeof(float));
        float position[3]{}, remainder = 1;
        for (unsigned b = 0; b <= draw.blend_weight_count; ++b) {
            const float weight = b == draw.blend_weight_count ? remainder : vertex[3+b];
            remainder -= weight;
            const float *m = draw.world[b];
            for (unsigned a = 0; a < 3; ++a)
                position[a] += weight*(vertex[0]*m[a]+vertex[1]*m[4+a]+vertex[2]*m[8+a]+m[12+a]);
        }
        for (unsigned a = 0; a < 3; ++a) {
            if (!std::isfinite(position[a])) return false;
            minimum[a] = (std::min)(minimum[a], position[a]);
            maximum[a] = (std::max)(maximum[a], position[a]);
        }
    }
    extent = (std::max)({maximum[0]-minimum[0], maximum[1]-minimum[1], maximum[2]-minimum[2]});
    return extent > 0 && extent < 200;
}

bool sunSameCamera(const SunShadow &sun, const RecompD3dPresenterDrawCommand &draw)
{
    for (unsigned i = 0; i < 16; ++i)
        if (!std::isfinite(draw.view_projection[i]) ||
            std::fabs(draw.view_projection[i]-sun.camera[i]) > 1e-4f*(1+std::fabs(sun.camera[i]))) return false;
    return true;
}

void sunSelectCamera(SunShadow &sun, const RecompD3dPresenterDrawCommand &draw)
{
    float extent, inverse[16], direction[3];
    if (draw.triangle_count <= sun.triangles || !sunSceneDraw(draw, extent) ||
        sunDirection(draw, direction) < 0 || !sunInverse(draw.view_projection, inverse)) return;
    sun.triangles = draw.triangle_count;
    sun.camera_epoch = sun.epoch;
    std::memcpy(sun.camera, draw.view_projection, sizeof sun.camera);
    std::memcpy(sun.inverse, inverse, sizeof inverse);
    std::memcpy(sun.direction, direction, sizeof direction);
}

bool sunFit(SunShadow &sun)
{
    float near_point[3], forward[3];
    const float *m = sun.inverse;
    const float w0 = m[15], w1 = 0.5f*m[11]+m[15];
    if (std::fabs(w0) < 1e-6f || std::fabs(w1) < 1e-6f) return false;
    for (unsigned i = 0; i < 3; ++i) {
        near_point[i] = m[12+i]/w0;
        forward[i] = (0.5f*m[8+i]+m[12+i])/w1-near_point[i];
    }
    if (!sunNormalize(forward)) return false;
    float right[3] = {sun.direction[2], 0, -sun.direction[0]};
    if (!sunNormalize(right)) { right[0] = 1; right[1] = right[2] = 0; }
    float up[3] = {sun.direction[1]*right[2]-sun.direction[2]*right[1],
        sun.direction[2]*right[0]-sun.direction[0]*right[2],
        sun.direction[0]*right[1]-sun.direction[1]*right[0]};
    float center[3];
    for (unsigned i = 0; i < 3; ++i) center[i] = near_point[i]+kSunAhead*forward[i];
    // shortcut: one 64-unit box 20 units down the view ray; extend to cascades for distant receivers.
    const float *axes[3] = {right, up, sun.direction};
    std::memset(sun.projection, 0, sizeof sun.projection);
    const float texel = kSunBox/kSunMapSize;
    for (unsigned c = 0; c < 3; ++c) {
        float offset = 0;
        const float scale = c == 2 ? 1.0f/kSunDepth : 2.0f/kSunBox;
        for (unsigned r = 0; r < 3; ++r) {
            sun.projection[r*4+c] = axes[c][r]*scale;
            offset += center[r]*axes[c][r];
        }
        if (c < 2) offset = std::round(offset/texel)*texel;
        sun.projection[12+c] = -offset*scale + (c == 2 ? 0.5f : 0);
    }
    sun.projection[15] = 1;
    for (float v : sun.projection) if (!std::isfinite(v)) return false;
    return true;
}

void sunRelease(SunShadow &sun)
{
    if (sun.rasterizer) sun.rasterizer->Release();
    if (sun.sampler) sun.sampler->Release();
    if (sun.constants) sun.constants->Release();
    if (sun.view) sun.view->Release();
    if (sun.depth) sun.depth->Release();
    if (sun.texture) sun.texture->Release();
    sun = {};
}

bool sunCreate(SunShadow &sun, ID3D11Device *device)
{
    if (!sun.enabled) return false;
    if (sun.texture) return true;
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = texture.Height = kSunMapSize;
    texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
    texture.Format = DXGI_FORMAT_R32_TYPELESS;
    texture.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC depth{};
    depth.Format = DXGI_FORMAT_D32_FLOAT;
    depth.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_FLOAT;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = 20*sizeof(float);
    constants.Usage = D3D11_USAGE_DYNAMIC;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    for (float &v : sampler.BorderColor) v = 1;
    sampler.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.DepthBias = 100;
    rasterizer.SlopeScaledDepthBias = 2;
    if (FAILED(device->CreateTexture2D(&texture, nullptr, &sun.texture)) ||
        FAILED(device->CreateDepthStencilView(sun.texture, &depth, &sun.depth)) ||
        FAILED(device->CreateShaderResourceView(sun.texture, &view, &sun.view)) ||
        FAILED(device->CreateBuffer(&constants, nullptr, &sun.constants)) ||
        FAILED(device->CreateSamplerState(&sampler, &sun.sampler)) ||
        FAILED(device->CreateRasterizerState(&rasterizer, &sun.rasterizer))) {
        sunRelease(sun);
        std::fprintf(stderr, "recomp sun shadow: resource creation failed; disabled\n");
        return false;
    }
    std::fprintf(stderr, "recomp sun shadow: %ux%u D32 comparison map\n", kSunMapSize, kSunMapSize);
    return true;
}

void sunShaderSource(std::string &source)
{
    source.insert(0, std::string(R"(
cbuffer SunShadow : register(b1) {
    row_major float4x4 sun_projection;
    float4 sun_flags;
};
Texture2D<float> sun_depth : register(t2);
SamplerComparisonState sun_sampler : register(s2);
static const float sun_texel = 1.0/)") + std::to_string(kSunMapSize) + R"(;
float sunVisibility(float3 p) {
    float3 q = mul(float4(p,1),sun_projection).xyz;
    if (any(abs(q.xy) >= 1) || q.z <= 0 || q.z >= 1) return 1;
    float2 uv = q.xy * float2(0.5,-0.5) + 0.5;
    float visibility = 0;
    [unroll] for (int y=-1; y<=1; ++y)
        [unroll] for (int x=-1; x<=1; ++x)
            visibility += sun_depth.SampleCmpLevelZero(sun_sampler,uv+float2(x,y)*sun_texel,q.z-0.0002);
    // Fade over the outer tenth of the box so shadows do not cut off at its edge.
    return lerp(1, visibility/9, saturate((1-max(abs(q.x),abs(q.y)))*10));
}
)");
    source.insert(source.find("struct VSOut {")+std::strlen("struct VSOut {"),
        "\n    float3 sun_world : TEXCOORD8;\n    float3 sun_direct : TEXCOORD9;\n    float3 sun_total : TEXCOORD10;\n");
    source.insert(source.find("    VSOut output;")+std::strlen("    VSOut output;"),
        "\n    output.sun_world=0; output.sun_direct=0; output.sun_total=0;\n");
    const auto contribution = source.find("            rgb += (directional_material.rgb");
    if (contribution != std::string::npos) {
        source.insert(contribution,
            "            if (sun_flags.x > 0 && i == (uint)sun_flags.y)\n"
            "                output.sun_direct = directional_material.rgb * directional_colors[i].rgb * max(0,dot(n,l));\n");
        // Normal offset (~2.5 texels) keeps grazing skin from shadowing itself at a low sun.
        source.insert(source.find("        output.color.rgb=saturate(rgb);"),
            "        output.sun_world=world_pos+(dot(n,n)>0 ? n*rsqrt(dot(n,n)) : 0)*0.04; output.sun_total=rgb;\n");
    }
    source.insert(source.find("    float4 shaded = input.color;"),
        "    if (sun_flags.x > 0) { float sun_v = sunVisibility(input.sun_world);\n"
        "        input.color.rgb = sun_flags.z > 0 ? input.color.rgb * (1 - sun_flags.z*(1-sun_v))\n"
        "            : saturate(input.sun_total - input.sun_direct * (1-sun_v)); }\n");
}

} // namespace
#endif
