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
    unsigned prop_shadows = 0;
    // Game shadow opacity (texture-factor alpha) seen last frame; 0 when the game drew none.
    float game_alpha = 0, seen_alpha = 0;
    // Caster pass marks rigid models (ball, props) in draw order; the visible pass
    // replays the same packet, so draw_index finds the flag without rescanning vertices.
    std::vector<uint8_t> compact;
    size_t draw_index = 0;
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

// direction receives light travel for the map from the adapter's sun (the game's own
// projected-shadow direction, world-space toward-light) on host-lit draws.
int sunDirection(const RecompD3dPresenterDrawCommand &draw, float direction[3])
{
    if (draw.sun[3] != 1 || !draw.directional.enabled) return -1;
    for (unsigned c = 0; c < 3; ++c) direction[c] = -draw.sun[c];
    if (const char *value = std::getenv("RECOMP_D3D_SUN_DIR")) {
        float override_direction[3];
        char trailing;
        if (std::sscanf(value, " %f , %f , %f %c", &override_direction[0],
                &override_direction[1], &override_direction[2], &trailing) == 3 &&
            sunNormalize(override_direction))
            std::memcpy(direction, override_direction, sizeof override_direction);
    }
    // A light at or below the horizon casts nothing useful; keep the game's shadow.
    if (direction[1] > -0.05f) return -1;
    return 0;
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

bool sunPropCombiner(const RecompD3dPresenterDrawCommand &draw)
{
    if (!draw.has_combiner) return true;
    const auto &a = draw.combiner[0];
    const auto &b = draw.combiner[1];
    return ((a[0] == 2 && a[2] == 2 && a[3] == 1) ||
        (a[0] == 4 && a[2] == 2 && a[3] == 0)) &&
        a[4] == 4 && a[6] == 2 && a[7] == 0 &&
        b[0] == 4 && b[2] == 3 && b[3] == 1 && b[4] == 4 && b[6] == 3 && b[7] == 1;
}

bool sunPropShadow(const RecompD3dPresenterDrawCommand &draw)
{
    const auto &d = draw.depth;
    const auto &b = draw.blend;
    return !draw.target.offscreen && !draw.target.no_depth && draw.has_texture &&
        !draw.texture_is_backbuffer && !draw.texture_is_frontbuffer && !draw.directional.enabled &&
        !draw.program_count && sunPropCombiner(draw) && !draw.has_alpha_mask && !draw.has_reflection &&
        !draw.four_tap_filter && !draw.use_texture_factor &&
        (draw.texture_factor & 0xffffffu) == 0xffffffu &&
        (draw.fvf == 0x112u || draw.fvf == 0x142u) &&
        d.alpha_test_enable && d.alpha_func == RECOMP_D3D_COMPARE_GREATER && d.alpha_ref == 1 &&
        d.depth_test_enable && d.depth_func == RECOMP_D3D_COMPARE_LESS_EQUAL &&
        d.stencil_enable && d.stencil_func == RECOMP_D3D_COMPARE_ALWAYS &&
        (d.stencil_ref == 0 || d.stencil_ref == 254) &&
        d.stencil_read_mask == 255 && d.stencil_write_mask == 255 &&
        d.stencil_fail == RECOMP_D3D_STENCIL_KEEP && d.stencil_zfail == RECOMP_D3D_STENCIL_KEEP &&
        d.stencil_pass == RECOMP_D3D_STENCIL_REPLACE && b.color_write_mask == 15 &&
        b.blend_enable && b.src_factor == RECOMP_D3D_BLEND_SRC_ALPHA &&
        b.dst_factor == RECOMP_D3D_BLEND_INV_SRC_ALPHA && b.op == RECOMP_D3D_BLEND_OP_ADD;
}

uint64_t sunPropPayloadHash(const void *bytes, uint32_t size)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    const auto *data = static_cast<const uint8_t *>(bytes);
    for (uint32_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}

bool sunStaticPropTexture(const RecompD3dTextureDesc &desc, uint64_t hash)
{
    if (desc.linear || desc.depth || desc.format_byte != RECOMP_D3D_TEXTURE_FORMAT_DXT5) return false;
    // shortcut: only verified static overlays; add identities after inspecting other courts.
    const struct { unsigned width, height, mips; uint64_t hash; } shadows[] = {
        {512, 128, 1, 0x945549f88c8cfe40ull}, // posts
        {256, 128, 8, 0xd99af6253b620e05ull},
        {512, 64, 7, 0x7135e45fe936c958ull},
        {256, 256, 1, 0xbc0a582f6161a6e7ull}, // palms
        {64, 128, 1, 0xbeb1f08e42f78a97ull},
        {128, 128, 1, 0xbd7d31e39cc51a98ull},
        {64, 128, 1, 0x6445a295ffd45cc5ull},
    };
    for (const auto &s : shadows)
        if (desc.width == s.width && desc.height == s.height &&
            desc.mip_levels == s.mips && hash == s.hash) return true;
    return false;
}

bool sunPalmTexture(const RecompD3dTextureDesc &desc)
{
    return !desc.linear && !desc.depth && desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8 &&
        desc.mip_levels <= 1 && ((desc.width == 512 && desc.height == 256) ||
        (desc.width == 256 && desc.height == 512));
}

bool sunPalmProducer(const RecompD3dPresenterDrawCommand &draw)
{
    return draw.target.offscreen && draw.target.no_depth && draw.fvf == 0x112 &&
        draw.has_texture && !draw.texture_is_backbuffer && !draw.texture_is_frontbuffer &&
        !draw.program_count && sunPropCombiner(draw) && !draw.has_alpha_mask && !draw.has_reflection &&
        (!draw.has_combiner || draw.combiner[0][0] == 4) &&
        !draw.four_tap_filter && draw.directional.enabled && draw.zero_diffuse_rgb &&
        !draw.use_texture_factor && !draw.modulate_texture_factor && draw.texture_factor == 0xffffffffu &&
        !draw.depth.depth_test_enable && !draw.depth.stencil_enable &&
        draw.depth.alpha_test_enable && draw.depth.alpha_func == RECOMP_D3D_COMPARE_GREATER &&
        draw.depth.alpha_ref == 1 && draw.blend.color_write_mask == 15 &&
        (!draw.blend.blend_enable || (draw.blend.src_factor == RECOMP_D3D_BLEND_SRC_ALPHA &&
        draw.blend.dst_factor == RECOMP_D3D_BLEND_INV_SRC_ALPHA && draw.blend.op == RECOMP_D3D_BLEND_OP_ADD));
}

struct SunPalmTarget {
    // Scene copies and reused targets must not inherit a silhouette's identity.
    bool cleared = false, silhouette = false;
    void clear(uint32_t color) { cleared = (color >> 24) == 0; silhouette = false; }
    void draw(const RecompD3dPresenterDrawCommand &draw) {
        if (!draw.blend.color_write_mask) return;
        if (sunPalmProducer(draw)) silhouette = true;
        else cleared = false;
    }
    bool valid() const { return cleared && silhouette; }
};

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

bool sunSceneDraw(const RecompD3dPresenterDrawCommand &draw, float size[3])
{
    RecompD3dVertexLayout layout;
    if (draw.target.offscreen || draw.target.no_depth || !draw.has_transform || draw.program_count ||
        !draw.vertex_bytes || !draw.index_bytes || !draw.vertex_count || !draw.index_count || !draw.triangle_count ||
        !draw.depth.depth_test_enable ||
        !recomp_d3d_fvf_layout(draw.fvf, &layout) || layout.pretransformed ||
        (layout.normal_offset == RECOMP_D3D_FVF_ABSENT && layout.diffuse_offset == RECOMP_D3D_FVF_ABSENT) ||
        layout.stride > draw.vertex_stride || draw.blend_weight_count > 3 ||
        (draw.blend_weight_count && draw.blend_weight_count != layout.blend_weight_count)) return false;
    float minimum[3] = {INFINITY, INFINITY, INFINITY}, maximum[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (unsigned i = 0; i < draw.index_count; ++i) {
        const unsigned v = static_cast<const uint16_t *>(draw.index_bytes)[i];
        if (v >= draw.vertex_count) return false;
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
    for (unsigned a = 0; a < 3; ++a) size[a] = maximum[a]-minimum[a];
    const float extent = (std::max)({size[0], size[1], size[2]});
    return extent > 0 && extent < 200;
}

bool sunCaster(const RecompD3dPresenterDrawCommand &draw, const float size[3])
{
    if (!draw.blend.color_write_mask) return false;
    const float extent = (std::max)({size[0], size[1], size[2]});
    const bool opaque = !draw.blend.blend_enable && draw.depth.depth_write_enable;
    if (draw.directional.enabled && opaque && (draw.blend_weight_count || extent <= 4)) return true;
    if (draw.blend_weight_count || !draw.has_texture || draw.texture.render_target ||
        draw.texture_is_backbuffer || draw.texture_is_frontbuffer) return false;
    // Fading unlit particles do not own scene depth.
    if (!draw.depth.depth_write_enable && !draw.directional.enabled) return false;
    const bool blended = draw.blend.blend_enable && draw.blend.src_factor == RECOMP_D3D_BLEND_SRC_ALPHA &&
        draw.blend.dst_factor == RECOMP_D3D_BLEND_INV_SRC_ALPHA && draw.blend.op == RECOMP_D3D_BLEND_OP_ADD;
    if (!opaque && !blended) return false;
    const float horizontal = (std::max)(size[0], size[2]);
    // The net mesh is a paper-thin sheet whose wires are finer than a map texel; they
    // reach the sand as a smear, so only its posts, tape and rope cast.
    if (blended && (std::min)(size[0], size[2]) <= horizontal*0.02f && horizontal > size[1]*2) return false;
    const bool cutout = draw.depth.alpha_test_enable && draw.depth.alpha_ref >= 128 &&
        (draw.depth.alpha_func == RECOMP_D3D_COMPARE_GREATER || draw.depth.alpha_func == RECOMP_D3D_COMPARE_GREATER_EQUAL);
    // Cutouts batch trees across the scene; flat receivers and broad opaque rocks must stay out.
    if (blended || cutout) return extent >= 0.5f && horizontal <= 128 &&
        size[1] >= horizontal*(blended ? 0.1f : 0.05f);
    // shortcut: narrow rigid geometry covers posts and net tape; add identity if other scenery shares these bounds.
    return horizontal <= 8 && (std::min)(size[0], size[2]) <= 1;
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
    float size[3], inverse[16], direction[3];
    RecompD3dVertexLayout layout;
    if (draw.triangle_count <= sun.triangles || !draw.depth.depth_write_enable ||
        !recomp_d3d_fvf_layout(draw.fvf, &layout) || layout.normal_offset == RECOMP_D3D_FVF_ABSENT ||
        layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT || !sunSceneDraw(draw, size) ||
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
    float2 uv = q.xy * float2(0.5,-0.5) + 0.5;
    float2 dx = ddx(uv), dy = ddy(uv);
    float2 dz = float2(ddx(q.z), ddy(q.z));
    float2 footprint = abs(dx) + abs(dy);
    if (any(abs(q.xy) >= 1) || q.z <= 0 || q.z >= 1) return 1;
    // One kernel at every distance: taps stay within two texels of each other so a
    // thin distant line cannot fall between them, and near shadows stay sharp.
    float2 spacing = clamp(footprint/5, 0.6*sun_texel, 2*sun_texel);
    float determinant = dx.x*dy.y-dx.y*dy.x;
    float2 slope = abs(determinant) > 1e-12 ?
        float2(dy.y*dz.x-dx.y*dz.y, dx.x*dz.y-dy.x*dz.x)/determinant : 0;
    // Each tap blends neighbouring texels, so sloped ground needs one texel of slope as bias.
    float bias = 0.0002 + dot(abs(slope), sun_texel);
    float visibility = 0;
    [unroll] for (int y=0; y<5; ++y)
        [unroll] for (int x=0; x<5; ++x) {
            float2 offset = (float2(x,y)-2)*spacing;
            visibility += sun_depth.SampleCmpLevelZero(sun_sampler,uv+offset,q.z+dot(slope,offset)-bias);
        }
    visibility /= 25;
    // Fade over the outer tenth of the box so shadows do not cut off at its edge.
    return lerp(1, visibility, saturate((1-max(abs(q.x),abs(q.y)))*10));
}
)");
    source.insert(source.find("struct VSOut {")+std::strlen("struct VSOut {"),
        "\n    float3 sun_world : TEXCOORD8;\n");
    source.insert(source.find("    VSOut output;")+std::strlen("    VSOut output;"),
        "\n    output.sun_world=0;\n");
    const char *anchor = "        if (directional_flags.y > 0.5f && dot(n,n)>0) n *= rsqrt(dot(n,n));\n";
    auto lit = source.find(anchor);
    if (lit != std::string::npos) lit += std::strlen(anchor);
    // Normal offset (~2.5 texels) keeps grazing surfaces from shadowing themselves at a low sun.
    if (lit != std::string::npos)
        source.insert(lit, "        output.sun_world=world_pos+(dot(n,n)>0 ? n*rsqrt(dot(n,n)) : 0)*0.04;\n");
    // Mip averages erase thin net wires before the alpha cutoff.
    source.insert(source.find("    if (draw_flags.y > 0.5f) {"),
        "    if (sun_flags.z > 0 && draw_flags.x > 0.5f)\n"
        "        shaded.a = guest_texture.SampleLevel(guest_sampler, input.texcoord * texture_flags.xy, 0).a;\n");
    source.insert(source.find("    float4 shaded = input.color;"),
        "    if (sun_flags.x > 0) input.color.rgb *= 1 - sun_flags.y*(1-sunVisibility(input.sun_world));\n");
}

} // namespace
#endif
