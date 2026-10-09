#include "d3d_presenter_d3d11_backend.h"
#include "d3d_draw_model.h"
#include "d3d_vblank.h"
#include "d3d_vertex_program.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>

#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <new>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <string>

#ifdef RECOMP_SMAA
#include "AreaTex.h"
#include "SearchTex.h"
static const char kSmaaSource[] = {
#include "smaa_hlsl.inc"
};
#endif

namespace {

constexpr wchar_t kWindowClassName[] = L"DOAXBVRecompPresenterWindow";
constexpr wchar_t kWindowTitle[] = L"DOAXBV Recomp";

struct FrameRateCounter {
    ULONGLONG start_ms = 0;
    uint32_t frames = 0;
    bool started = false;
};

bool sampleFrameRate(FrameRateCounter &counter, ULONGLONG now, double &fps, double &ms)
{
    if (!counter.started) {
        counter.started = true;
        counter.start_ms = now;
        return false;
    }
    ++counter.frames;
    const ULONGLONG elapsed = now - counter.start_ms;
    if (elapsed < 1000u) return false;
    fps = counter.frames * 1000.0 / elapsed;
    ms = static_cast<double>(elapsed) / counter.frames;
    counter.start_ms = now;
    counter.frames = 0;
    return true;
}


/* Untransformed positions with a host-side world-view-projection composite.

   Vertex components other than position vary by FVF: this title draws both
   XYZ|NORMAL|TEX1 and XYZ|DIFFUSE|TEX1, whose texcoords sit at different
   offsets and whose colors come from different sources. One shader cannot
   serve both, so the shader is assembled per FVF from the decoded layout and
   cached with its matching input layout.

   Vertex color comes from the stream or the admitted fixed-function lighting
   path. Unsupported lighting configurations retain the earlier texture/normal
   fallback. Texture alpha remains independent of lit diffuse RGB. */
constexpr char kDrawShaderPrologue[] =
    "cbuffer Transform : register(b0) {\n"
    "    row_major float4x4 wvp[4];\n"
    "    float4 draw_flags;\n"
    "    float4 blend_flags;\n"
    "    float4 texture_factor;\n"
    "    float4 lighting_flags;\n"
    "    float4 texture_flags;\n"
    "    row_major float4x4 reflection_world_view;\n"
    "    row_major float4x4 reflection_normal;\n"
    "    row_major float4x4 reflection_transform;\n"
    "    float4 reflection_diffuse;\n"
    "    float4 reflection_flags;\n"
    "    float4 vc[192];\n"
    "    row_major float4x4 directional_normals[4];\n"
    "    float4 directional_base;\n"
    "    float4 directional_material;\n"
    "    float4 directional_directions[8];\n"
    "    float4 directional_colors[8];\n"
    "    float4 directional_flags;\n"
    "    row_major float4x4 light_world[4];\n"
    "    float4 light_positions[8];\n"
    "    float4 light_attenuation[8];\n"
    "    float4 light_ambient[8];\n"
    "    float4 fog_color;\n"
    "    float4 fog_params; // start, end, density, mode\n"
    "    float4 fog_flags; // enabled, Z depth, range, visualization\n"
    "    row_major float4x4 fog_world_view[4];\n"
    "}\n"
    "float fogFactor(float depth) {\n"
    "    if (fog_params.w == 1) return saturate(exp(-depth * fog_params.z));\n"
    "    if (fog_params.w == 2) { float d = depth * fog_params.z; return saturate(exp(-d*d)); }\n"
    "    if (fog_params.w == 3) {\n"
    "        float span = fog_params.y - fog_params.x;\n"
    "        return span == 0 ? (depth < fog_params.y ? 1 : 0) : saturate((fog_params.y-depth)/span);\n"
    "    }\n"
    "    return saturate(depth);\n"
    "}\n"
    "Texture2D guest_texture : register(t0);\n"
    "SamplerState guest_sampler : register(s0);\n"
    "Texture2D alpha_mask : register(t1);\n"
    "SamplerState mask_sampler : register(s1);\n";

/* Builds the draw shader for one decoded vertex layout. Only the components
   the layout actually carries appear in VSIn, so the input layout and the
   shader signature always agree. */
void buildDrawShaderSource(
    const RecompD3dVertexLayout &layout,
    char *out,
    size_t out_size)
{
    const bool has_normal =
        layout.normal_offset != RECOMP_D3D_FVF_ABSENT;
    const bool has_diffuse =
        layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT;
    const bool has_texcoord =
        layout.texcoord_offset != RECOMP_D3D_FVF_ABSENT;
    const bool four_coords = layout.texcoord_count == 4u;
    const char *extra_coords = four_coords
        ? "    float2 texcoord1 : TEXCOORD1;\n"
          "    float2 texcoord2 : TEXCOORD2;\n"
          "    float2 texcoord3 : TEXCOORD3;\n"
        : layout.texcoord_count == 2u ? "    float2 texcoord1 : TEXCOORD1;\n" : "";

    char position[768];
    if (layout.pretransformed) {
        std::snprintf(position, sizeof position,
            "    output.position = mul(float4(input.position.xyz, 1.0f), wvp[0]) / input.position.w;\n");
    } else if (layout.blend_weight_count != 0u) {
        std::snprintf(position, sizeof position,
            "    if (blend_flags.x > 0.5f) {\n"
            "    output.position = float4(0, 0, 0, 0);\n"
            "    float remainder = 1.0f;\n"
            "    [unroll] for (uint i = 0; i < %u; ++i) {\n"
            "        output.position += input.weights[i] * mul(float4(input.position, 1), wvp[i]);\n"
            "        remainder -= input.weights[i];\n"
            "    }\n"
            "    output.position += remainder * mul(float4(input.position, 1), wvp[%u]);\n"
            "    } else { output.position = mul(float4(input.position, 1), wvp[0]); }\n",
            layout.blend_weight_count, layout.blend_weight_count);
    } else {
        std::snprintf(position, sizeof position,
            "    output.position = mul(float4(input.position, 1.0f), wvp[0]);\n");
    }
    std::snprintf(
        out,
        out_size,
        "%s"
        "struct VSIn {\n"
        "    %s position : POSITION;\n"
        "%s%s%s%s%s"
        "};\n"
        "struct VSOut {\n"
        "    float4 position : SV_POSITION;\n"
        "    float4 color : COLOR0;\n"
        "    float2 texcoord : TEXCOORD0;\n"
        "    float2 reflection_coord : TEXCOORD4;\n"
        "%s"
        "};\n"
        "VSOut vs_main(VSIn input) {\n"
        "    VSOut output;\n"
        "%s"
        "%s"
        "%s"
        "%s"
        "    output.reflection_coord = float2(0, 0);\n"
        "%s"
        "    return output;\n"
        "}\n"
        "float4 ps_main(VSOut input) : SV_TARGET {\n"
        "    float4 shaded = input.color;\n"
        "    if (reflection_flags.x > 0.5f) {\n"
        "        float4 base = guest_texture.Sample(guest_sampler, input.texcoord);\n"
        "        base.a *= reflection_diffuse.a;\n"
        "        float4 env = alpha_mask.Sample(guest_sampler, input.reflection_coord);\n"
        "        float4 diffuse = reflection_diffuse;\n"
        "        if (directional_flags.x > 0.5f) diffuse.rgb = input.color.rgb;\n"
        "        shaded = lerp(base, env, env.a) * diffuse;\n"
        "    } else\n"
        "%s"
        "    if (blend_flags.y == 1.0f) {\n"
        "        shaded = texture_factor;\n"
        "    } else if (draw_flags.x > 0.5f) {\n"
        "        float4 sampled = guest_texture.Sample(guest_sampler, input.texcoord * texture_flags.xy);\n"
        "        if (lighting_flags.y > 0.5f) sampled.rgb = 1.0f;\n"
        "        shaded %s sampled;\n"
        "        if (directional_flags.x > 0.5f) shaded.rgb *= input.color.rgb;\n"
        "        if (lighting_flags.x > 0.5f) shaded.rgb = 0.0f;\n"
        "        shaded.a = blend_flags.w > 0.5f\n"
        "            ? blend_flags.z : shaded.a * blend_flags.z;\n"
        "        if (blend_flags.y > 1.5f) shaded *= texture_factor;\n"
        "    }\n"
        "    if (reflection_flags.z > 0.5f) shaded.a *= alpha_mask.SampleBias(mask_sampler, input.reflection_coord, reflection_flags.w).a;\n"
        /* NV2A compares rounded 8-bit alpha. Function values follow
           RecompD3dCompareFunc, from NEVER (0) to ALWAYS (7). */
        "    if (draw_flags.y > 0.5f) {\n"
        "        float alpha = round(saturate(shaded.a) * 255.0f);\n"
        "        float ref = draw_flags.w;\n"
        "        int func = (int)draw_flags.z;\n"
        "        bool alpha_pass = func == 7 ||\n"
        "            (func == 1 && alpha < ref) ||\n"
        "            (func == 2 && alpha == ref) ||\n"
        "            (func == 3 && alpha <= ref) ||\n"
        "            (func == 4 && alpha > ref) ||\n"
        "            (func == 5 && alpha != ref) ||\n"
        "            (func == 6 && alpha >= ref);\n"
        "        if (!alpha_pass) discard;\n"
        "    }\n"
        "    if (fog_flags.x > 0.5f) {\n"
        "        float depth = fog_flags.y > 0.5f && fog_flags.z < 0.5f ? abs(input.position.z) : input.fog_depth;\n"
        "        float factor = fog_params.w == 0 ? saturate(input.fog_factor) : fogFactor(depth);\n"
        "        shaded.rgb = fog_flags.w > 0.5f ? factor.xxx : lerp(fog_color.rgb, shaded.rgb, factor);\n"
        "    }\n"
        "    return shaded;\n"
        "}\n",
        kDrawShaderPrologue,
        layout.pretransformed ? "float4" : "float3",
        layout.blend_weight_count ? "    float3 weights : BLENDWEIGHT;\n" : "",
        has_normal ? "    float3 normal : NORMAL;\n" : "",
        has_diffuse ? "    float4 diffuse : COLOR0;\n" : "",
        has_texcoord ? "    float2 texcoord : TEXCOORD0;\n" : "",
        extra_coords,
        extra_coords,
        position,
        /* Guest vertex color when the stream has one; otherwise the normal
           stands in so surfaces remain distinguishable. */
        has_diffuse
            ? "    output.color = input.diffuse;\n"
            : (has_normal
                   ? "    output.color = "
                     "float4(abs(normalize(input.normal)), 1.0f);\n"
                   : "    output.color = float4(0.75f, 0.75f, 0.78f, 1.0f);\n"),
        has_texcoord
            ? "    output.texcoord = input.texcoord;\n"
            : "    output.texcoord = float2(0.0f, 0.0f);\n",
        four_coords
            ? "    output.texcoord1 = input.texcoord1;\n"
              "    output.texcoord2 = input.texcoord2;\n"
              "    output.texcoord3 = input.texcoord3;\n"
            : layout.texcoord_count == 2u ? "    output.texcoord1 = input.texcoord1;\n" : "",
        has_texcoord && !layout.pretransformed
            ? (has_normal
                ? "    if (reflection_flags.x > 1.5f) {\n"
                  "        output.reflection_coord = input.texcoord;\n"
                  "    } else if (reflection_flags.x > 0.5f) {\n"
                  "        float3 eye = mul(float4(input.position, 1), reflection_world_view).xyz;\n"
                  "        float3 n = mul(float4(input.normal, 0), reflection_normal).xyz;\n"
                  "        if (reflection_flags.y > 0.5f) n = normalize(n);\n"
                  "        float3 r = reflect(normalize(eye), n);\n"
                  "        output.reflection_coord = mul(float4(r, 1), reflection_transform).xy;\n"
                  "    }\n"
                : "    if (reflection_flags.x > 1.5f) {\n"
                  "        output.reflection_coord = input.texcoord;\n"
                  "    } else if (reflection_flags.x > 0.5f) {\n"
                  "        float3 eye = mul(float4(input.position, 1), reflection_world_view).xyz;\n"
                  "        float3 r = normalize(eye);\n"
                  "        output.reflection_coord = mul(float4(r, 1), reflection_transform).xy;\n"
                  "    }\n")
            : "",
        four_coords
            ? "    if (texture_flags.z > 0.5f) {\n"
              "        float4 t0 = guest_texture.Sample(guest_sampler, input.texcoord * texture_flags.xy);\n"
              "        float4 t1 = guest_texture.Sample(guest_sampler, input.texcoord1 * texture_flags.xy);\n"
              "        float4 t2 = guest_texture.Sample(guest_sampler, input.texcoord2 * texture_flags.xy);\n"
              "        float4 t3 = guest_texture.Sample(guest_sampler, input.texcoord3 * texture_flags.xy);\n"
              "        shaded = 0.5f * (saturate((128.0f / 255.0f) * (t0 + t1))\n"
              "                        + saturate((128.0f / 255.0f) * (t2 + t3)));\n"
              "    } else\n"
            : layout.texcoord_count == 2u
            ? "    if (texture_flags.w > 0.5f) {\n"
              "        float3 rgb = guest_texture.Sample(guest_sampler, input.texcoord1 * texture_flags.xy).rgb;\n"
              "        float alpha = alpha_mask.Sample(mask_sampler, input.texcoord * lighting_flags.zw).a;\n"
              "        shaded = input.color * float4(rgb, alpha);\n"
              "    } else\n" : "",
        /* The fixed-function diffuse default is white. Preserve texture alpha
           before the shared alpha test and SRC_ALPHA blending. */
        has_diffuse ? "*=" : "=");
}

LRESULT CALLBACK presenterWindowProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam);

template <typename T>
void releaseCom(T *&object)
{
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

/* Compiled state for one FVF. */
/* ponytail: 16 pipelines cover the measured ten-pipeline pool working set.
   Revisit the bound if another scene exceeds it and causes compilation churn. */
constexpr uint32_t kDrawPipelineSlots = 16u;

struct DrawPipeline {
    uint32_t fvf;
    uint32_t program_count;
    uint32_t program[136][4];
    bool used;
    bool failed;
    ID3D11VertexShader *vertex_shader;
    ID3D11PixelShader *pixel_shader;
    ID3D11InputLayout *input_layout;
};

/* Depth-stencil states are few and repeat every frame, so they are cached by
   the guest state that produced them rather than rebuilt per draw. */
constexpr uint32_t kDepthStateSlots = 16u;

struct DepthStateEntry {
    bool used;
    bool test_enable;
    bool write_enable;
    RecompD3dCompareFunc func;
    bool stencil_enable;
    RecompD3dCompareFunc stencil_func;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    RecompD3dStencilOp stencil_fail;
    RecompD3dStencilOp stencil_zfail;
    RecompD3dStencilOp stencil_pass;
    ID3D11DepthStencilState *state;
};

constexpr uint32_t kBlendStateSlots = 16u;

struct BlendStateEntry {
    bool used;
    bool enable;
    RecompD3dBlendFactor src;
    RecompD3dBlendFactor dst;
    RecompD3dBlendOp op;
    uint8_t color_write_mask;
    ID3D11BlendState *state;
};

/* Guest textures are cached by address and shape. Linear movie buffers are
   rewritten in place, so their pixels are refreshed whenever sampled.
   Map and menu screens sample about 300 textures per frame; a FIFO smaller
   than one frame's set evicts each texture before its next use and rebuilds
   all of them every frame. */
constexpr uint32_t kTextureSlots = 4096u;
constexpr uint32_t kPaletteBytes = 256u * 4u;

struct TextureEntry {
    bool used;
    uint32_t data;
    uint32_t format_byte;
    uint32_t width;
    uint32_t height;
    uint32_t mip_levels;
    uint8_t palette[kPaletteBytes];
    uint64_t fingerprint;
    ID3D11ShaderResourceView *view;
};

/* A guest buffer can be refilled with another texture of the same size and
   format without being freed (item previews reuse theirs), so a hit must also
   match the texels.
   ponytail: samples 128 words; a rewrite missing all of them stays stale.
   Hash the whole span if that is ever observed. */
uint64_t textureFingerprint(const void *bytes, uint32_t count)
{
    const auto *data = static_cast<const uint8_t *>(bytes);
    uint64_t hash = 0xcbf29ce484222325ull ^ count;
    if (count < 8u) {
        for (uint32_t i = 0u; i < count; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
        return hash;
    }
    constexpr uint32_t kSamples = 128u;
    for (uint32_t i = 0u; i < kSamples; ++i) {
        uint64_t word;
        std::memcpy(&word, data + static_cast<uint64_t>(count - 8u) * i / (kSamples - 1u), 8u);
        hash = (hash ^ word) * 0x100000001b3ull;
    }
    return hash;
}

/* Rendered pixels have no CPU copy to re-upload after texture FIFO eviction.
   Retain targets until their guest backing storage is released.
   ponytail: bound host target texels to 64 MiB; guest-arena suballocation
   retirement is needed if retained scene targets exhaust this byte budget. */
constexpr uint64_t kTargetByteLimit = 64u * 1024u * 1024u;

struct RenderTargetEntry {
    RecompD3dTextureDesc desc;
    ID3D11RenderTargetView *render_view;
    ID3D11ShaderResourceView *sample_view;
    float scale = 1.0f;  // host texels per guest texel
    uint64_t bytes = 0u;
};

/* Depth belongs to its guest storage, which can serve several color targets. */

struct DepthTargetEntry {
    RecompD3dTextureDesc desc;
    ID3D11DepthStencilView *view;
    bool host_main;  // created at the scaled/MSAA main target size
    uint64_t bytes;
};

} // namespace

struct DumpTextureRelease {
    void operator()(ID3D11Texture2D *texture) const { if (texture) texture->Release(); }
};
struct DeferredFrameDump {
    std::unique_ptr<ID3D11Texture2D, DumpTextureRelease> texture;
    std::string path;
    unsigned present;
    ULONGLONG captured_ms;
};

struct RecompD3dPresenter {
    RecompD3dPresenterConfig config{};
    DWORD owner_thread = 0;
    HINSTANCE instance = nullptr;
    bool owns_window_class = false;
    HWND window = nullptr;
    bool close_requested = false;
    bool widescreen = false;
    float scale = 1.0f;         // RECOMP_D3D_SCALE: main target height multiplier
    uint32_t msaa = 1u;         // RECOMP_D3D_MSAA: main target sample count
    float target_scale_x = 1.0f, target_scale_y = 1.0f; // bound target / guest size
    bool smaa = false;          // RECOMP_D3D_SMAA: post-process the output
    ID3D11VertexShader *smaa_vs[3]{};   // edges, weights, blend
    ID3D11PixelShader *smaa_ps[3]{};
    ID3D11ShaderResourceView *smaa_views[6]{}; // -, edges, area, search, weights, output
    ID3D11RenderTargetView *smaa_targets[3]{}; // edges, weights, output
    ID3D11SamplerState *smaa_samplers[2]{};   // linear, point
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    IDXGISwapChain *swap_chain = nullptr;
    ID3D11RenderTargetView *render_target_view = nullptr;
    ID3D11RenderTargetView *present_target_view = nullptr;
    // VRR only: main-sized output, downscaled into the window-sized swap chain.
    ID3D11RenderTargetView *vrr_target_view = nullptr;
    ID3D11ShaderResourceView *vrr_source = nullptr;
    ID3D11VertexShader *vrr_vs = nullptr;
    ID3D11PixelShader *vrr_ps = nullptr;
    ID3D11SamplerState *vrr_sampler = nullptr;
    ID3D11VertexShader *gamma_vertex_shader = nullptr;
    ID3D11PixelShader *gamma_pixel_shader = nullptr;
    ID3D11Buffer *gamma_buffer = nullptr;
    bool gamma_enabled = false;
    ID3D11Texture2D *back_buffer_copy = nullptr;
    ID3D11ShaderResourceView *back_buffer_sample = nullptr;
    ID3D11VertexShader *target_copy_vs = nullptr;
    ID3D11PixelShader *target_copy_ps = nullptr;
    ID3D11Texture2D *front_buffer_copy = nullptr;
    ID3D11ShaderResourceView *front_buffer_sample = nullptr;
    ID3D11Texture2D *depth_texture = nullptr;
    ID3D11DepthStencilView *depth_view = nullptr;
    ID3D11Buffer *draw_vertex_buffer = nullptr;
    ID3D11Buffer *draw_index_buffer = nullptr;
    ID3D11Buffer *draw_constant_buffer = nullptr;
    ID3D11RasterizerState *draw_rasterizer_states[3] = {};
    UINT draw_vertex_capacity = 0u;
    UINT draw_index_capacity = 0u;
    UINT draw_vertex_used = 0u;
    UINT draw_index_used = 0u;
    /* One pipeline per FVF. A failure is recorded against its own FVF so a
       stream this seam cannot build never disables the ones it can. */
    DrawPipeline draw_pipelines[kDrawPipelineSlots]{};
    uint32_t draw_pipeline_count = 0u;
    uint32_t next_draw_pipeline_slot = 0u;
    std::atomic<bool> precompile_stop{false};
    std::thread precompile;
    DepthStateEntry depth_states[kDepthStateSlots]{};
    uint32_t depth_state_count = 0u;
    uint32_t next_depth_state_slot = 0u;
    BlendStateEntry blend_states[kBlendStateSlots]{};
    uint32_t blend_state_count = 0u;
    uint32_t next_blend_state_slot = 0u;
    std::vector<TextureEntry> textures = std::vector<TextureEntry>(kTextureSlots);
    std::unordered_multimap<uint32_t, uint32_t> texture_index; // guest address -> slot
    uint32_t texture_count = 0u;
    uint32_t next_texture_slot = 0u;
    std::vector<RenderTargetEntry> render_targets;
    std::vector<DepthTargetEntry> depth_targets;
    uint64_t target_bytes = 0u;
    ID3D11SamplerState *draw_sampler = nullptr;
    ID3D11SamplerState *address_samplers[4][4]{};
    ID3D11SamplerState *filter_sampler = nullptr;
    ID3D11SamplerState *program_mask_sampler = nullptr;
    bool draw_shared_ready = false;
    bool draw_shared_failed = false;
    uint32_t draw_count = 0u;
    bool first_draw_reported = false;
    const char *driver_name = "unknown";
    HRESULT create_result = E_FAIL;
    uint32_t present_count = 0;
    bool performance_counter = false;
    FrameRateCounter frame_rate;
    // RECOMP_PERF_COUNTER pacing: present-to-present gaps within the second.
    double last_present_ms = 0.0;
    double present_call_max_ms = 0.0;
    std::vector<double> present_gaps;
    // Display refreshes each present stayed on screen (DXGI statistics), 1..8+.
    unsigned refresh_holds[9]{};
    UINT last_stat_present = 0u, last_stat_refresh = 0u;
    /* RECOMP_D3D_PRESENT_LOG: one CSV row of raw DXGI counters per present,
       in QPC ticks, to line up with a PresentMon --qpc_time capture. */
    FILE *present_log = nullptr;
    UINT sync_interval = 1u;
    std::chrono::steady_clock::time_point next_refresh_check{};
    bool vrr = false;           // RECOMP_D3D_VRR=1: the game's 60 Hz timer paces a VRR display
    long long vrr_slot_ns = 0;  // steady_clock time of the last VRR present slot
    bool first_present_reported = false;
    unsigned frame_dump_count = 0u;
    unsigned frame_dump_burst_base = 0u;
    unsigned frame_dump_burst_count = 0u;
    std::vector<DeferredFrameDump> deferred_dumps;
    // Staging textures created when a triggered burst starts, so capturing
    // during the burst is only a GPU copy and does not disturb pacing.
    std::vector<std::unique_ptr<ID3D11Texture2D, DumpTextureRelease>> dump_pool;
    uint32_t replay_verify_frame = 0;
    bool replay_verify_second = false;
    std::vector<uint8_t> replay_reference;
    ULONGLONG next_frame_dump_ms = 0u;
};

namespace {

RecompD3dPresenter *active_presenter;

static bool immediate_present;
static bool split_presentation;

/* kernel_config.c reports the Xbox widescreen video flag unless
   RECOMP_D3D_WIDESCREEN=0, so the game renders anamorphic 16:9 (or 4:3) into
   the guest backbuffer; the window presents that buffer at the same aspect. */
uint32_t presentClientWidth(const RecompD3dPresenter *presenter, uint64_t height)
{
    return static_cast<uint32_t>(presenter->widescreen
        ? (height * 16u + 8u) / 9u
        : (height * 4u + 1u) / 3u);
}

/* A scaled main target renders at the display aspect, so the anamorphic guest
   width gets full horizontal detail too. Scale 1 keeps the guest size. */
uint32_t mainHeight(const RecompD3dPresenter *presenter)
{
    return static_cast<uint32_t>(presenter->config.height * presenter->scale + 0.5f);
}

uint32_t mainWidth(const RecompD3dPresenter *presenter)
{
    return presenter->scale == 1.0f
        ? presenter->config.width : presentClientWidth(presenter, mainHeight(presenter));
}

/* Beyond the screen the swap chain downsamples into a screen-sized window. */
uint32_t windowHeight(const RecompD3dPresenter *presenter)
{
    return presenter->scale == 1.0f ? presenter->config.height : (std::min)(
        mainHeight(presenter), static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN)));
}

/* VRR needs direct flip, which a DWM-scaled swap chain loses; with VRR the
   swap chain matches the window and the presenter scales into it itself. */
bool immediatePresent(const RecompD3dPresenter *presenter)
{
    return immediate_present && !presenter->vrr;
}

bool vrrScaled(const RecompD3dPresenter *presenter)
{
    return presenter->vrr &&
        (mainWidth(presenter) != presentClientWidth(presenter, windowHeight(presenter)) ||
         mainHeight(presenter) != windowHeight(presenter));
}

LRESULT CALLBACK presenterWindowProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        const auto *creation = reinterpret_cast<const CREATESTRUCTW *>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    if (message == WM_CLOSE) {
        auto *presenter = reinterpret_cast<RecompD3dPresenter *>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (presenter != nullptr) {
            presenter->close_requested = true;
        }
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool configSupported(const RecompD3dPresenterConfig &config)
{
    return config.width != 0u && config.height != 0u &&
        config.color_format ==
            RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM &&
        config.depth_format == RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8;
}

void releaseSmaa(RecompD3dPresenter *presenter)
{
    for (auto &shader : presenter->smaa_vs) releaseCom(shader);
    for (auto &shader : presenter->smaa_ps) releaseCom(shader);
    for (auto &view : presenter->smaa_views) releaseCom(view);
    for (auto &view : presenter->smaa_targets) releaseCom(view);
    for (auto &sampler : presenter->smaa_samplers) releaseCom(sampler);
}

void releaseGraphics(RecompD3dPresenter *presenter)
{
    if (presenter->context != nullptr) {
        presenter->context->OMSetRenderTargets(0u, nullptr, nullptr);
        presenter->context->ClearState();
        presenter->context->Flush();
    }
    releaseCom(presenter->present_target_view);
    releaseCom(presenter->vrr_target_view);
    releaseCom(presenter->vrr_source);
    releaseCom(presenter->vrr_vs);
    releaseCom(presenter->vrr_ps);
    releaseCom(presenter->vrr_sampler);
    releaseCom(presenter->gamma_vertex_shader);
    releaseCom(presenter->gamma_pixel_shader);
    releaseCom(presenter->gamma_buffer);
    releaseSmaa(presenter);
    for (auto &state : presenter->draw_rasterizer_states) releaseCom(state);
    releaseCom(presenter->draw_constant_buffer);
    releaseCom(presenter->draw_index_buffer);
    releaseCom(presenter->draw_vertex_buffer);
    for (uint32_t i = 0u; i < presenter->draw_pipeline_count; ++i) {
        DrawPipeline &pipeline = presenter->draw_pipelines[i];

        releaseCom(pipeline.input_layout);
        releaseCom(pipeline.pixel_shader);
        releaseCom(pipeline.vertex_shader);
        pipeline.used = false;
    }
    presenter->draw_pipeline_count = 0u;
    presenter->next_draw_pipeline_slot = 0u;
    for (uint32_t i = 0u; i < presenter->depth_state_count; ++i) {
        DepthStateEntry &entry = presenter->depth_states[i];

        releaseCom(entry.state);
        entry.used = false;
    }
    presenter->depth_state_count = 0u;
    presenter->next_depth_state_slot = 0u;
    for (uint32_t i = 0u; i < presenter->blend_state_count; ++i) {
        BlendStateEntry &entry = presenter->blend_states[i];

        releaseCom(entry.state);
        entry.used = false;
    }
    presenter->blend_state_count = 0u;
    presenter->next_blend_state_slot = 0u;
    for (uint32_t i = 0u; i < presenter->texture_count; ++i) {
        TextureEntry &entry = presenter->textures[i];

        releaseCom(entry.view);
        entry.used = false;
    }
    presenter->texture_index.clear();
    presenter->texture_count = 0u;
    presenter->next_texture_slot = 0u;
    for (uint32_t i = 0u; i < presenter->render_targets.size(); ++i) {
        releaseCom(presenter->render_targets[i].sample_view);
        releaseCom(presenter->render_targets[i].render_view);
    }
    presenter->render_targets.clear();
    for (uint32_t i = 0u; i < presenter->depth_targets.size(); ++i) {
        releaseCom(presenter->depth_targets[i].view);
    }
    presenter->depth_targets.clear();
    presenter->target_bytes = 0u;
    releaseCom(presenter->draw_sampler);
    for (auto &row : presenter->address_samplers) {
        for (auto &sampler : row) releaseCom(sampler);
    }
    releaseCom(presenter->filter_sampler);
    releaseCom(presenter->program_mask_sampler);
    presenter->draw_vertex_capacity = 0u;
    presenter->draw_index_capacity = 0u;
    presenter->draw_vertex_used = 0u;
    presenter->draw_index_used = 0u;
    presenter->draw_shared_ready = false;
    releaseCom(presenter->depth_view);
    releaseCom(presenter->depth_texture);
    releaseCom(presenter->render_target_view);
    releaseCom(presenter->back_buffer_sample);
    releaseCom(presenter->back_buffer_copy);
    releaseCom(presenter->target_copy_vs);
    releaseCom(presenter->target_copy_ps);
    releaseCom(presenter->front_buffer_sample);
    releaseCom(presenter->front_buffer_copy);
    releaseCom(presenter->swap_chain);
    releaseCom(presenter->context);
    releaseCom(presenter->device);
}

void flushFrameDumps(RecompD3dPresenter *presenter);

void releasePresenter(RecompD3dPresenter *presenter)
{
    flushFrameDumps(presenter);
    releaseGraphics(presenter);
    if (presenter->present_log != nullptr) {
        std::fclose(presenter->present_log);
        presenter->present_log = nullptr;
    }
    if (presenter->window != nullptr && IsWindow(presenter->window)) {
        DestroyWindow(presenter->window);
    }
    presenter->window = nullptr;
    if (presenter->owns_window_class) {
        UnregisterClassW(kWindowClassName, presenter->instance);
        presenter->owns_window_class = false;
    }
}

bool createWindow(RecompD3dPresenter *presenter)
{
    WNDCLASSEXW window_class{};
    const uint32_t client_width = presentClientWidth(presenter, windowHeight(presenter));
    RECT window_rect = {
        0,
        0,
        static_cast<LONG>(client_width),
        static_cast<LONG>(windowHeight(presenter)),
    };
    /* A scaled window fills the screen; decorations would not fit. */
    const DWORD style = presenter->scale > 1.0f ? WS_POPUP : WS_OVERLAPPEDWINDOW;

    presenter->instance = GetModuleHandleW(nullptr);
    window_class.cbSize = sizeof window_class;
    window_class.lpfnWndProc = presenterWindowProc;
    window_class.hInstance = presenter->instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Resource 1 is the optional RECOMP_APP_ICON; without it Windows uses its default.
    window_class.hIcon = static_cast<HICON>(LoadImageW(presenter->instance, MAKEINTRESOURCEW(1),
        IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(presenter->instance, MAKEINTRESOURCEW(1),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    window_class.lpszClassName = kWindowClassName;

    const ATOM window_class_atom = RegisterClassExW(&window_class);
    if (window_class_atom == 0u) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    } else {
        presenter->owns_window_class = true;
    }
    if (!AdjustWindowRectEx(&window_rect, style, FALSE, 0u)) {
        return false;
    }

    presenter->window = CreateWindowExW(
        0u,
        kWindowClassName,
        kWindowTitle,
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        nullptr,
        nullptr,
        presenter->instance,
        presenter);
    if (presenter->window == nullptr) {
        return false;
    }

    RECT client_rect{};
    return GetClientRect(presenter->window, &client_rect) &&
        client_rect.right - client_rect.left ==
            static_cast<LONG>(client_width) &&
        client_rect.bottom - client_rect.top ==
            static_cast<LONG>(windowHeight(presenter));
}

HRESULT createDeviceWithDriver(
    RecompD3dPresenter *presenter,
    D3D_DRIVER_TYPE driver_type)
{
    const bool immediate = immediatePresent(presenter);
    DXGI_SWAP_CHAIN_DESC swap_chain_desc{};
    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected_feature_level{};

    swap_chain_desc.BufferDesc.Width = vrrScaled(presenter)
        ? presentClientWidth(presenter, windowHeight(presenter)) : mainWidth(presenter);
    swap_chain_desc.BufferDesc.Height = vrrScaled(presenter)
        ? windowHeight(presenter) : mainHeight(presenter);
    swap_chain_desc.BufferDesc.RefreshRate.Numerator = 0u;
    swap_chain_desc.BufferDesc.RefreshRate.Denominator = 1u;
    swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap_chain_desc.SampleDesc.Count = 1u;
    swap_chain_desc.SampleDesc.Quality = 0u;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = immediate ? 1u :
        split_presentation ? 3u : 2u;
    swap_chain_desc.OutputWindow = presenter->window;
    swap_chain_desc.Windowed = TRUE;
    swap_chain_desc.SwapEffect = immediate
        ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.Flags = immediate ? 0u : DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (presenter->vrr) swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        driver_type,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        feature_levels,
        static_cast<UINT>(sizeof feature_levels / sizeof feature_levels[0]),
        D3D11_SDK_VERSION,
        &swap_chain_desc,
        &presenter->swap_chain,
        &presenter->device,
        &selected_feature_level,
        &presenter->context);
    // Flip chains use the per-chain limit; immediate presents retain the default.
    if (SUCCEEDED(result) && !immediate) {
        IDXGISwapChain2 *chain = nullptr;
        result = presenter->swap_chain->QueryInterface(IID_PPV_ARGS(&chain));
        if (SUCCEEDED(result)) result = chain->SetMaximumFrameLatency(1u);
        releaseCom(chain);
    }
    return result;
}

HRESULT createGraphics(RecompD3dPresenter *presenter)
{
    HRESULT hardware_result = createDeviceWithDriver(
        presenter, D3D_DRIVER_TYPE_HARDWARE);

    if (SUCCEEDED(hardware_result)) {
        presenter->driver_name = "hardware";
        presenter->create_result = hardware_result;
    } else {
        std::fprintf(
            stderr,
            "recomp d3d presenter: hardware create failed hr=0x%08lX; "
            "trying WARP\n",
            static_cast<unsigned long>(hardware_result));
        releaseGraphics(presenter);
        const HRESULT warp_result = createDeviceWithDriver(
            presenter, D3D_DRIVER_TYPE_WARP);
        if (FAILED(warp_result)) {
            std::fprintf(
                stderr,
                "recomp d3d presenter: WARP create failed hr=0x%08lX\n",
                static_cast<unsigned long>(warp_result));
            return warp_result;
        }
        presenter->driver_name = "warp";
        presenter->create_result = warp_result;
        std::fprintf(stderr, "recomp d3d presenter: using WARP driver\n");
    }
    ID3D11Texture2D *back_buffer = nullptr;
    HRESULT result = presenter->swap_chain->GetBuffer(
        0u,
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void **>(&back_buffer));
    const UINT requested_msaa = presenter->msaa;
    presenter->msaa = 1u;
    for (UINT count = 2u; count <= 32u; count *= 2u) {
        UINT quality = 0u;
        presenter->device->CheckMultisampleQualityLevels(
            DXGI_FORMAT_B8G8R8A8_UNORM, count, &quality);
        UINT depth_quality = 0u;
        presenter->device->CheckMultisampleQualityLevels(
            DXGI_FORMAT_D24_UNORM_S8_UINT, count, &depth_quality);
        std::fprintf(stderr, "recomp d3d presenter: msaa %ux quality color=%u depth=%u\n",
            count, quality, depth_quality);
        if (count <= requested_msaa && quality != 0u && depth_quality != 0u) {
            presenter->msaa = count;
        }
    }
    std::fprintf(stderr, "recomp d3d presenter: target=%ux%u msaa=%u\n",
        mainWidth(presenter), mainHeight(presenter), presenter->msaa);
    if (SUCCEEDED(result)) {
        result = presenter->device->CreateRenderTargetView(
            back_buffer, nullptr, &presenter->present_target_view);
    }
    /* Keep guest pixels uncorrected: gamma is display state and must not feed
       back into later draws or compound when a frame is presented twice. */
    if (SUCCEEDED(result)) {
        D3D11_TEXTURE2D_DESC desc{};
        back_buffer->GetDesc(&desc);
        desc.Width = mainWidth(presenter);
        desc.Height = mainHeight(presenter);
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = 0u;
        desc.SampleDesc.Count = presenter->msaa;
        ID3D11Texture2D *guest_buffer = nullptr;
        result = presenter->device->CreateTexture2D(&desc, nullptr, &guest_buffer);
        if (SUCCEEDED(result)) {
            result = presenter->device->CreateRenderTargetView(
                guest_buffer, nullptr, &presenter->render_target_view);
        }
        releaseCom(guest_buffer);
    }
    if (SUCCEEDED(result) && vrrScaled(presenter)) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = mainWidth(presenter);
        desc.Height = mainHeight(presenter);
        desc.MipLevels = desc.ArraySize = 1u;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1u;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D *output = nullptr;
        result = presenter->device->CreateTexture2D(&desc, nullptr, &output);
        if (SUCCEEDED(result)) result = presenter->device->CreateRenderTargetView(
            output, nullptr, &presenter->vrr_target_view);
        if (SUCCEEDED(result)) result = presenter->device->CreateShaderResourceView(
            output, nullptr, &presenter->vrr_source);
        releaseCom(output);
        static const char shader[] =
            "Texture2D pixels : register(t0);\n"
            "SamplerState bilinear : register(s0);\n"
            "float4 vs(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position {\n"
            " uv = float2((id << 1) & 2, id & 2);\n"
            " return float4(uv * float2(2,-2) + float2(-1,1), 0, 1); }\n"
            "float4 ps(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
            " return pixels.SampleLevel(bilinear, uv, 0); }\n";
        ID3DBlob *vertex = nullptr, *pixel = nullptr;
        if (SUCCEEDED(result)) result = D3DCompile(shader, sizeof shader - 1u, nullptr,
            nullptr, nullptr, "vs", "vs_4_0", 0u, 0u, &vertex, nullptr);
        if (SUCCEEDED(result)) result = D3DCompile(shader, sizeof shader - 1u, nullptr,
            nullptr, nullptr, "ps", "ps_4_0", 0u, 0u, &pixel, nullptr);
        if (SUCCEEDED(result)) result = presenter->device->CreateVertexShader(
            vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &presenter->vrr_vs);
        if (SUCCEEDED(result)) result = presenter->device->CreatePixelShader(
            pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &presenter->vrr_ps);
        releaseCom(vertex);
        releaseCom(pixel);
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (SUCCEEDED(result)) result = presenter->device->CreateSamplerState(
            &sampler, &presenter->vrr_sampler);
    }
    releaseCom(back_buffer);
    if (FAILED(result)) {
        return result;
    }

    D3D11_TEXTURE2D_DESC depth_desc{};
    depth_desc.Width = mainWidth(presenter);
    depth_desc.Height = mainHeight(presenter);
    depth_desc.MipLevels = 1u;
    depth_desc.ArraySize = 1u;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = presenter->msaa;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    result = presenter->device->CreateTexture2D(
        &depth_desc, nullptr, &presenter->depth_texture);
    if (FAILED(result)) {
        return result;
    }
    result = presenter->device->CreateDepthStencilView(
        presenter->depth_texture, nullptr, &presenter->depth_view);
    if (FAILED(result)) {
        return result;
    }

    presenter->context->OMSetRenderTargets(
        1u, &presenter->render_target_view, presenter->depth_view);
    const D3D11_VIEWPORT viewport = {
        0.0f,
        0.0f,
        static_cast<float>(mainWidth(presenter)),
        static_cast<float>(mainHeight(presenter)),
        0.0f,
        1.0f,
    };
    presenter->context->RSSetViewports(1u, &viewport);
    presenter->target_scale_x = viewport.Width / presenter->config.width;
    presenter->target_scale_y = viewport.Height / presenter->config.height;
    return S_OK;
}

RenderTargetEntry *findRenderTarget(
    RecompD3dPresenter *presenter,
    const RecompD3dTextureDesc &desc)
{
    for (uint32_t i = 0u; i < presenter->render_targets.size(); ++i) {
        RenderTargetEntry &entry = presenter->render_targets[i];

        if (entry.desc.data == desc.data &&
            entry.desc.format_byte == desc.format_byte &&
            entry.desc.width == desc.width && entry.desc.height == desc.height) {
            return &entry;
        }
    }
    return nullptr;
}

RecompD3dPresenterError lookupDepthTarget(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterTarget &target,
    uint32_t color_width,
    uint32_t color_height,
    ID3D11DepthStencilView *&view)
{
    view = nullptr;
    if (target.no_depth) {
        return RECOMP_D3D_PRESENTER_OK;
    }
    const RecompD3dTextureDesc &desc = target.depth;
    const uint32_t width = target.custom_depth
        ? desc.width : presenter->config.width;
    const uint32_t height = target.custom_depth
        ? desc.height : presenter->config.height;
    if (width != color_width || height != color_height) {
        std::fprintf(stderr,
            "recomp d3d presenter: target size mismatch "
            "color=0x%08X fmt=0x%02X size=%ux%u "
            "depth=0x%08X fmt=0x%02X size=%ux%u custom_depth=%d\n",
            target.color.data, target.color.format_byte, color_width, color_height,
            desc.data, desc.format_byte, width, height,
            target.custom_depth ? 1 : 0);
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (!target.custom_depth) {
        view = presenter->depth_view;
        return RECOMP_D3D_PRESENTER_OK;
    }
    if (!desc.depth || desc.data == 0u || width == 0u || height == 0u ||
        width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        (desc.format_byte != 0x2au && desc.format_byte != 0x2eu)) {
        std::fprintf(stderr,
            "recomp d3d presenter: unsupported depth target data=0x%08X "
            "fmt=0x%02X size=%ux%u depth=%d\n",
            desc.data, desc.format_byte, width, height, desc.depth ? 1 : 0);
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    /* A custom depth bound with the main target must match its host size and samples. */
    const bool host_main = !target.offscreen &&
        (presenter->scale != 1.0f || presenter->msaa > 1u);
    const uint32_t host_width = host_main ? mainWidth(presenter) : width;
    const uint32_t host_height = host_main ? mainHeight(presenter) : height;
    const uint32_t samples = host_main ? presenter->msaa : 1u;
    for (uint32_t i = 0u; i < presenter->depth_targets.size(); ++i) {
        const DepthTargetEntry &entry = presenter->depth_targets[i];
        if (entry.desc.data == desc.data &&
            entry.desc.format_byte == desc.format_byte &&
            entry.desc.width == width && entry.desc.height == height &&
            entry.host_main == host_main) {
            view = entry.view;
            return RECOMP_D3D_PRESENTER_OK;
        }
    }
    // Like the main color target, a main-sized depth is outside the offscreen budget.
    const uint64_t bytes = host_main
        ? 0u : static_cast<uint64_t>(host_width) * host_height * 4u * samples;
    if (bytes > kTargetByteLimit - presenter->target_bytes) {
        std::fprintf(stderr, "recomp d3d presenter: target memory budget exhausted\n");
        return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    }

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = host_width;
    texture_desc.Height = host_height;
    texture_desc.MipLevels = 1u;
    texture_desc.ArraySize = 1u;
    texture_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    texture_desc.SampleDesc.Count = samples;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D *texture = nullptr;
    HRESULT result = presenter->device->CreateTexture2D(
        &texture_desc, nullptr, &texture);
    if (SUCCEEDED(result)) {
        result = presenter->device->CreateDepthStencilView(
            texture, nullptr, &view);
    }
    releaseCom(texture);
    if (FAILED(result)) {
        releaseCom(view);
        std::fprintf(stderr,
            "recomp d3d presenter: depth target create failed "
            "data=0x%08X fmt=0x%02X size=%ux%u hr=0x%08lX\n",
            desc.data, desc.format_byte, width, height,
            static_cast<unsigned long>(result));
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    try {
        presenter->depth_targets.push_back({desc, view, host_main, bytes});
    } catch (const std::bad_alloc &) {
        releaseCom(view);
        return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    }
    presenter->target_bytes += bytes;
    std::fprintf(stderr,
        "recomp d3d presenter: depth target data=0x%08X fmt=0x%02X size=%ux%u\n",
        desc.data, desc.format_byte, host_width, host_height);
    return RECOMP_D3D_PRESENTER_OK;
}

bool resampleTarget(RecompD3dPresenter *presenter, ID3D11ShaderResourceView *source,
    ID3D11RenderTargetView *target, uint32_t width, uint32_t height);

RecompD3dPresenterError bindTarget(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterTarget &target,
    ID3D11RenderTargetView *&color_view,
    ID3D11DepthStencilView *&depth_view,
    bool scene_copy = false)
{
    color_view = presenter->render_target_view;
    uint32_t width = presenter->config.width;
    uint32_t height = presenter->config.height;
    float offscreen_scale = 1.0f;

    if (target.offscreen) {
        const RecompD3dTextureDesc &desc = target.color;
        if (desc.depth || desc.data == 0u ||
            desc.width == 0u || desc.height == 0u ||
            desc.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            desc.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            (desc.format_byte != RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8 &&
             desc.format_byte != 0x12u)) {
            std::fprintf(stderr,
                "recomp d3d presenter: unsupported target data=0x%08X "
                "fmt=0x%02X size=%ux%u no_depth=%d\n",
                desc.data, desc.format_byte, desc.width, desc.height,
                target.no_depth ? 1 : 0);
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }

        RenderTargetEntry *entry = findRenderTarget(presenter, desc);
        /* Only new held-frame targets without depth start at main scale.
           Existing targets retain both their pixels and allocation size.
           ponytail: size heuristic; key on the consumer if a 512 target blurs. */
        float scale = scene_copy && target.no_depth && desc.width >= 512u && desc.height >= 512u
            ? (std::min)(static_cast<float>(mainHeight(presenter)) / presenter->config.height,
                  static_cast<float>(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) /
                      (std::max)(desc.width, desc.height))
            : 1.0f;
        if (static_cast<uint64_t>(desc.width * scale) * static_cast<uint64_t>(desc.height * scale) * 4u >
            kTargetByteLimit - presenter->target_bytes) scale = 1.0f;
        // Restore guest size before binding depth, which belongs to shared guest storage.
        const bool restore_depth = entry != nullptr && entry->scale != 1.0f && !target.no_depth;
        if (restore_depth) scale = 1.0f;
        if (entry == nullptr || restore_depth) {
            const uint32_t host_width = static_cast<uint32_t>(desc.width * scale);
            const uint32_t host_height = static_cast<uint32_t>(desc.height * scale);
            const uint64_t bytes = static_cast<uint64_t>(host_width) * host_height * 4u;
            const uint64_t replaced_bytes = restore_depth ? entry->bytes : 0u;
            if (bytes > kTargetByteLimit - (presenter->target_bytes - replaced_bytes)) {
                std::fprintf(stderr, "recomp d3d presenter: target memory budget exhausted\n");
                return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
            }
            D3D11_TEXTURE2D_DESC texture_desc{};
            texture_desc.Width = host_width;
            texture_desc.Height = host_height;
            texture_desc.MipLevels = 1u;
            texture_desc.ArraySize = 1u;
            texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            texture_desc.SampleDesc.Count = 1u;
            texture_desc.Usage = D3D11_USAGE_DEFAULT;
            texture_desc.BindFlags =
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            ID3D11Texture2D *texture = nullptr;
            RenderTargetEntry created{};
            HRESULT result = presenter->device->CreateTexture2D(
                &texture_desc, nullptr, &texture);
            if (SUCCEEDED(result)) {
                result = presenter->device->CreateRenderTargetView(
                    texture, nullptr, &created.render_view);
            }
            if (SUCCEEDED(result)) {
                result = presenter->device->CreateShaderResourceView(
                    texture, nullptr, &created.sample_view);
            }
            releaseCom(texture);
            if (FAILED(result)) {
                releaseCom(created.sample_view);
                releaseCom(created.render_view);
                std::fprintf(stderr,
                    "recomp d3d presenter: render target create failed "
                    "data=0x%08X fmt=0x%02X size=%ux%u hr=0x%08lX\n",
                    desc.data, desc.format_byte, desc.width, desc.height,
                    static_cast<unsigned long>(result));
                return RECOMP_D3D_PRESENTER_HOST_FAILURE;
            }
            created.desc = desc;
            created.scale = scale;
            created.bytes = bytes;
            if (restore_depth) {
                if (!resampleTarget(presenter, entry->sample_view, created.render_view, host_width, host_height)) {
                    releaseCom(created.sample_view); releaseCom(created.render_view);
                    return RECOMP_D3D_PRESENTER_HOST_FAILURE;
                }
                presenter->target_bytes -= entry->bytes;
                releaseCom(entry->sample_view); releaseCom(entry->render_view);
                *entry = created;
            } else {
                try {
                    presenter->render_targets.push_back(created);
                } catch (const std::bad_alloc &) {
                    releaseCom(created.sample_view); releaseCom(created.render_view);
                    return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
                }
                entry = &presenter->render_targets.back();
            }
            presenter->target_bytes += bytes;
            std::fprintf(stderr,
                "recomp d3d presenter: render target data=0x%08X "
                "fmt=0x%02X size=%ux%u host=%ux%u\n",
                desc.data, desc.format_byte, desc.width, desc.height,
                host_width, host_height);
        }
        color_view = entry->render_view;
        width = desc.width;
        height = desc.height;
        offscreen_scale = entry->scale;
    }

    const RecompD3dPresenterError depth_result =
        lookupDepthTarget(presenter, target, width, height, depth_view);
    if (depth_result != RECOMP_D3D_PRESENTER_OK) {
        return depth_result;
    }
    /* ponytail: an offscreen target cannot share the scaled/MSAA main depth;
       it draws without depth. Give it its own depth if one ever needs it. */
    if (target.offscreen && depth_view == presenter->depth_view &&
        (presenter->scale != 1.0f || presenter->msaa > 1u)) {
        depth_view = nullptr;
    }
    const float host_width = static_cast<float>(
        target.offscreen ? static_cast<uint32_t>(width * offscreen_scale) : mainWidth(presenter));
    const float host_height = static_cast<float>(
        target.offscreen ? static_cast<uint32_t>(height * offscreen_scale) : mainHeight(presenter));
    presenter->target_scale_x = host_width / width;
    presenter->target_scale_y = host_height / height;

    /* Clear can write a previously sampled target too. Switch the output
       before rebinding t0 so sampling the previous output remains valid. */
    ID3D11ShaderResourceView *no_texture = nullptr;
    presenter->context->PSSetShaderResources(0u, 1u, &no_texture);
    presenter->context->OMSetRenderTargets(1u, &color_view, depth_view);
    const D3D11_VIEWPORT viewport = {
        0.0f, 0.0f, host_width, host_height,
        0.0f, 1.0f,
    };
    presenter->context->RSSetViewports(1u, &viewport);
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError submitClear(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterClearCommand &clear)
{
    if ((clear.clear_depth &&
         (!std::isfinite(clear.z) || clear.z < 0.0f || clear.z > 1.0f)) ||
        (clear.clear_stencil && clear.stencil > 0xffu)) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }

    ID3D11RenderTargetView *color_view;
    ID3D11DepthStencilView *depth_view;
    const RecompD3dPresenterError target_result =
        bindTarget(presenter, clear.target, color_view, depth_view);
    if (target_result != RECOMP_D3D_PRESENTER_OK) {
        return target_result;
    }

    constexpr float byte_to_float = 1.0f / 255.0f;
    const float color[] = {
        static_cast<float>((clear.color >> 16u) & 0xffu) * byte_to_float,
        static_cast<float>((clear.color >> 8u) & 0xffu) * byte_to_float,
        static_cast<float>(clear.color & 0xffu) * byte_to_float,
        static_cast<float>((clear.color >> 24u) & 0xffu) * byte_to_float,
    };
    if (clear.clear_color) {
        presenter->context->ClearRenderTargetView(
            color_view, color);
    }
    UINT depth_stencil_flags = 0u;
    if (clear.clear_depth) {
        depth_stencil_flags |= D3D11_CLEAR_DEPTH;
    }
    if (clear.clear_stencil) {
        depth_stencil_flags |= D3D11_CLEAR_STENCIL;
    }
    /* The guest ignores depth/stencil clear bits when no depth is attached. */
    if (depth_stencil_flags != 0u && depth_view != nullptr) {
        presenter->context->ClearDepthStencilView(
            depth_view,
            depth_stencil_flags,
            clear.z,
            static_cast<UINT8>(clear.stencil));
    }
    return FAILED(presenter->device->GetDeviceRemovedReason())
        ? RECOMP_D3D_PRESENTER_HOST_FAILURE
        : RECOMP_D3D_PRESENTER_OK;
}

/* Compiled draw shaders, keyed by entry point and source. Shared by the boot
   precompile thread and the render worker; blobs live until exit. */
std::mutex compiled_shaders_lock;
std::unordered_map<std::string, ID3DBlob *> compiled_shaders;

bool compileDrawShader(
    const char *source,
    const char *entry_point,
    const char *target,
    ID3DBlob **blob)
{
    std::string key = std::string(entry_point) + '\n' + source;
    {
        std::lock_guard<std::mutex> lock(compiled_shaders_lock);
        const auto found = compiled_shaders.find(key);
        if (found != compiled_shaders.end()) {
            *blob = found->second;
            (*blob)->AddRef();
            return true;
        }
    }

    ID3DBlob *errors = nullptr;
    const HRESULT result = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry_point,
        target,
        0u,
        0u,
        blob,
        &errors);

    if (FAILED(result)) {
        std::fprintf(
            stderr,
            "recomp d3d presenter: draw shader %s failed hr=0x%08lX %s\n",
            entry_point,
            static_cast<unsigned long>(result),
            errors != nullptr
                ? static_cast<const char *>(errors->GetBufferPointer())
                : "");
    }
    releaseCom(errors);
    if (SUCCEEDED(result)) {
        std::lock_guard<std::mutex> lock(compiled_shaders_lock);
        if (compiled_shaders.emplace(std::move(key), *blob).second) {
            (*blob)->AddRef();
        }
    }
    return SUCCEEDED(result);
}

/* Builds the HLSL for one FVF, plus its vertex program when it has one. */
bool drawShaderSource(
    uint32_t fvf,
    const DrawPipeline &pipeline,
    RecompD3dVertexLayout &layout,
    std::string &compiled_source)
{
    if (!recomp_d3d_fvf_layout(fvf, &layout)) {
        return false;
    }

    char source[12288];
    buildDrawShaderSource(layout, source, sizeof source);
    compiled_source = source;
    const auto fields = compiled_source.find("struct VSOut {") + std::strlen("struct VSOut {");
    compiled_source.insert(fields, "\n    float fog_depth : TEXCOORD6;\n"
        "    noperspective float fog_factor : TEXCOORD7;\n");
    if (layout.specular_offset != RECOMP_D3D_FVF_ABSENT) {
        const auto input = compiled_source.find("struct VSIn {") + std::strlen("struct VSIn {");
        compiled_source.insert(input, "\n    float4 specular : COLOR1;\n");
    }
    std::string fog = "    output.fog_depth = fog_flags.y > 0.5f ? abs(output.position.z) : output.position.w;\n";
    fog += layout.specular_offset != RECOMP_D3D_FVF_ABSENT
        ? "    output.fog_factor = input.specular.a;\n" : "    output.fog_factor = 1;\n";
    if (!layout.pretransformed) {
        fog += "    if (fog_flags.z > 0.5f) {\n"
            "        float3 eye = mul(float4(input.position,1),fog_world_view[0]).xyz;\n";
        if (layout.blend_weight_count) {
            fog += "        if (blend_flags.x > 0.5f) { eye=0; float remainder=1;\n";
            for (uint32_t i = 0; i < layout.blend_weight_count; ++i) {
                const auto index = std::to_string(i);
                fog += "eye += input.weights["+index+"] * mul(float4(input.position,1),fog_world_view["+index+"]).xyz;\n"
                    "remainder -= input.weights["+index+"];\n";
            }
            fog += "eye += remainder * mul(float4(input.position,1),fog_world_view["+
                std::to_string(layout.blend_weight_count)+"]).xyz; }\n";
        }
        fog += "        output.fog_depth = length(eye);\n    }\n";
    } else {
        fog += "    if (fog_flags.y > 0.5f) output.fog_depth = abs(output.position.z / output.position.w);\n";
    }
    compiled_source.insert(compiled_source.find("    return output;"), fog);
    if (layout.normal_offset != RECOMP_D3D_FVF_ABSENT && !layout.pretransformed &&
        layout.diffuse_offset == RECOMP_D3D_FVF_ABSENT) {
        std::string lighting = "    if (directional_flags.x > 0.5f) {\n"
            "        float3 n = mul(float4(input.normal,0),directional_normals[0]).xyz;\n"
            "        float3 world_pos = mul(float4(input.position,1),light_world[0]).xyz;\n";
        if (layout.blend_weight_count) {
            lighting += "        if (blend_flags.x > 0.5f) { n=0; world_pos=0; float remainder=1;\n";
            for (uint32_t i=0; i<layout.blend_weight_count; ++i) {
                const auto index=std::to_string(i);
                lighting += "n += input.weights["+index+"] * mul(float4(input.normal,0),directional_normals["+index+"]).xyz;\n"
                    "world_pos += input.weights["+index+"] * mul(float4(input.position,1),light_world["+index+"]).xyz;\n"
                    "remainder -= input.weights["+index+"];\n";
            }
            lighting += "n += remainder * mul(float4(input.normal,0),directional_normals["+
                std::to_string(layout.blend_weight_count)+"]).xyz;\n"
                "world_pos += remainder * mul(float4(input.position,1),light_world["+
                std::to_string(layout.blend_weight_count)+"]).xyz; }\n";
        }
        lighting += "        if (directional_flags.y > 0.5f && dot(n,n)>0) n *= rsqrt(dot(n,n));\n"
            "        float3 rgb=directional_base.rgb;\n"
            "        for (uint i=0; i<(uint)directional_flags.z; ++i) {\n"
            "            float3 l = directional_directions[i].xyz; float atten = 1;\n"
            "            if (light_positions[i].w > 0.5f) {\n"
            "                l = light_positions[i].xyz - world_pos;\n"
            "                float d = length(l);\n"
            "                float denominator = dot(light_attenuation[i].xyz,float3(1,d,d*d));\n"
            "                atten = d <= light_attenuation[i].w && denominator > 0 ? 1/denominator : 0;\n"
            "                l = d > 0 ? l/d : float3(0,0,0);\n"
            "            }\n"
            "            rgb += (directional_material.rgb * directional_colors[i].rgb * max(0,dot(n,l)) + light_ambient[i].rgb) * atten;\n"
            "        }\n"
            "        output.color.rgb=saturate(rgb);\n"
            "    }\n";
        const auto uv = compiled_source.find("    output.texcoord =");
        if (uv == std::string::npos) return false;
        compiled_source.insert(uv, lighting);
    }
    if (pipeline.program_count) {
        if (fvf != 0x112u) return false;
        std::string body;
        if (!recomp_d3d_vertex_program_source(pipeline.program, pipeline.program_count, body)) return false;
        const auto begin = compiled_source.find("VSOut vs_main(");
        const auto end = compiled_source.find("float4 ps_main(", begin);
        if (begin == std::string::npos || end == std::string::npos) return false;
        compiled_source.replace(begin, end-begin, "VSOut vs_main(VSIn input) { VSOut output;\n" + body + "return output; }\n");
        // Preserve q until the pixel shader so projection follows interpolation.
        const std::string declaration = "struct VSOut {";
        const auto fields = compiled_source.find(declaration);
        if (fields == std::string::npos) return false;
        compiled_source.insert(fields + declaration.size(), "\n    float2 program_q : TEXCOORD5;\n");
        const std::string pixel = "float4 ps_main(VSOut input) : SV_TARGET {";
        const auto sample = compiled_source.find(pixel);
        if (sample == std::string::npos) return false;
        compiled_source.insert(sample + pixel.size(),
            "\n    input.texcoord /= input.program_q.x;\n"
            "    if (reflection_flags.z > 0.5f) input.reflection_coord /= input.program_q.y;\n");

    }
    return true;
}

/* Every FVF seen across all logged runs (14). Compiling them at boot keeps
   first use of each scene from stalling ~85 ms per shader. */
constexpr uint32_t kBootDrawFvfs[] = {
    0x042u, 0x104u, 0x112u, 0x116u, 0x118u, 0x11Au,
    0x142u, 0x144u, 0x212u, 0x216u, 0x21Au, 0x242u, 0x244u, 0x404u};

// ponytail: fixed list; vertex-program shaders and unlisted FVFs still compile on first use.
void precompileDrawShaders(const std::atomic<bool> *stop)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    const ULONGLONG start = GetTickCount64();
    static const DrawPipeline no_program{};
    uint32_t compiled = 0u;
    try {
        for (const uint32_t fvf : kBootDrawFvfs) {
            RecompD3dVertexLayout layout;
            std::string source;
            if (!drawShaderSource(fvf, no_program, layout, source)) continue;
            for (const char *stage : {"vs", "ps"}) {
                if (stop->load()) return;
                ID3DBlob *blob = nullptr;
                const bool vertex = stage[0] == 'v';
                compiled += compileDrawShader(source.c_str(), vertex ? "vs_main" : "ps_main",
                    vertex ? "vs_4_0" : "ps_4_0", &blob);
                releaseCom(blob);
            }
        }
    } catch (const std::exception &) {
        return;  // Remaining shaders compile on first use.
    }
    std::fprintf(stderr, "recomp d3d presenter: precompiled %u draw shaders in %llu ms\n",
        compiled, static_cast<unsigned long long>(GetTickCount64() - start));
}

/* Builds the shader pair and input layout for one FVF from its decoded
   component offsets. */
bool createDrawPipeline(
    RecompD3dPresenter *presenter,
    uint32_t fvf,
    DrawPipeline &pipeline)
{
    RecompD3dVertexLayout layout;
    std::string compiled_source;
    if (!drawShaderSource(fvf, pipeline, layout, compiled_source)) {
        return false;
    }

    ID3DBlob *vertex_blob = nullptr;
    ID3DBlob *pixel_blob = nullptr;
    if (!compileDrawShader(compiled_source.c_str(), "vs_main", "vs_4_0", &vertex_blob) ||
        !compileDrawShader(compiled_source.c_str(), "ps_main", "ps_4_0", &pixel_blob)) {
        releaseCom(vertex_blob);
        releaseCom(pixel_blob);
        return false;
    }

    HRESULT result = presenter->device->CreateVertexShader(
        vertex_blob->GetBufferPointer(),
        vertex_blob->GetBufferSize(),
        nullptr,
        &pipeline.vertex_shader);
    if (SUCCEEDED(result)) {
        result = presenter->device->CreatePixelShader(
            pixel_blob->GetBufferPointer(),
            pixel_blob->GetBufferSize(),
            nullptr,
            &pipeline.pixel_shader);
    }
    if (SUCCEEDED(result)) {
        D3D11_INPUT_ELEMENT_DESC elements[9]{};
        UINT count = 0u;

        elements[count++] = {
            "POSITION", 0u, layout.pretransformed
                ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32G32B32_FLOAT, 0u,
            layout.position_offset, D3D11_INPUT_PER_VERTEX_DATA, 0u};
        if (layout.blend_weight_count != 0u) {
            const DXGI_FORMAT formats[] = {
                DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
                DXGI_FORMAT_R32G32B32_FLOAT};
            elements[count++] = {
                "BLENDWEIGHT", 0u, formats[layout.blend_weight_count - 1u], 0u,
                12u, D3D11_INPUT_PER_VERTEX_DATA, 0u};
        }
        if (layout.normal_offset != RECOMP_D3D_FVF_ABSENT) {
            elements[count++] = {
                "NORMAL", 0u, DXGI_FORMAT_R32G32B32_FLOAT, 0u,
                layout.normal_offset, D3D11_INPUT_PER_VERTEX_DATA, 0u};
        }
        if (layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT) {
            /* Xbox diffuse is packed A8R8G8B8, so the BGRA host format
               delivers the channels in the order the shader expects. */
            elements[count++] = {
                "COLOR", 0u, DXGI_FORMAT_B8G8R8A8_UNORM, 0u,
                layout.diffuse_offset, D3D11_INPUT_PER_VERTEX_DATA, 0u};
        }
        if (layout.specular_offset != RECOMP_D3D_FVF_ABSENT) {
            elements[count++] = {
                "COLOR", 1u, DXGI_FORMAT_B8G8R8A8_UNORM, 0u,
                layout.specular_offset, D3D11_INPUT_PER_VERTEX_DATA, 0u};
        }
        const uint32_t texture_coords = layout.texcoord_count == 4u ? 4u
            : layout.texcoord_count == 2u ? 2u
            : (layout.texcoord_count != 0u ? 1u : 0u);
        for (uint32_t i = 0u; i < texture_coords; ++i) {
            elements[count++] = {
                "TEXCOORD", i, DXGI_FORMAT_R32G32_FLOAT, 0u,
                layout.texcoord_offset + i * 8u, D3D11_INPUT_PER_VERTEX_DATA, 0u};
        }
        result = presenter->device->CreateInputLayout(
            elements,
            count,
            vertex_blob->GetBufferPointer(),
            vertex_blob->GetBufferSize(),
            &pipeline.input_layout);
    }
    releaseCom(vertex_blob);
    releaseCom(pixel_blob);
    if (FAILED(result)) {
        std::fprintf(
            stderr,
            "recomp d3d presenter: draw pipeline fvf=0x%08X failed "
            "hr=0x%08lX\n",
            static_cast<unsigned>(fvf),
            static_cast<unsigned long>(result));
        releaseCom(pipeline.input_layout);
        releaseCom(pipeline.pixel_shader);
        releaseCom(pipeline.vertex_shader);
        return false;
    }
    std::fprintf(
        stderr,
        "recomp d3d presenter: draw pipeline fvf=0x%08X stride=%u "
        "normal=%d diffuse=%d texcoord=%d\n",
        static_cast<unsigned>(fvf),
        static_cast<unsigned>(layout.stride),
        layout.normal_offset != RECOMP_D3D_FVF_ABSENT
            ? static_cast<int>(layout.normal_offset) : -1,
        layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT
            ? static_cast<int>(layout.diffuse_offset) : -1,
        layout.texcoord_offset != RECOMP_D3D_FVF_ABSENT
            ? static_cast<int>(layout.texcoord_offset) : -1);
    return true;
}

/* Device state every draw shares, independent of vertex format. */
bool ensureSharedDrawState(RecompD3dPresenter *presenter)
{
    if (presenter->draw_shared_ready) {
        return true;
    }
    if (presenter->draw_shared_failed) {
        return false;
    }
    presenter->draw_shared_failed = true;

    HRESULT result;
    D3D11_BUFFER_DESC constant_desc{};
    /* WVP/blend transforms and draw flags (140 floats), vc[192], then lighting:
       normal transforms, material/base, directions/colors/flags, world transforms,
       point positions, attenuation/range and material-scaled ambient (300 floats). */
    constant_desc.ByteWidth = (140u + 192u * 4u + 300u + 76u) * sizeof(float);
    constant_desc.Usage = D3D11_USAGE_DYNAMIC;
    constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constant_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = presenter->device->CreateBuffer(
        &constant_desc, nullptr, &presenter->draw_constant_buffer);
    if (FAILED(result)) {
        return false;
    }

    for (unsigned mode = 0; mode < 3; ++mode) {
        D3D11_RASTERIZER_DESC rasterizer_desc{};
        rasterizer_desc.FillMode = D3D11_FILL_SOLID;
        rasterizer_desc.CullMode = mode == RECOMP_D3D_CULL_NONE
            ? D3D11_CULL_NONE : D3D11_CULL_BACK;
        rasterizer_desc.FrontCounterClockwise = mode == RECOMP_D3D_CULL_CLOCKWISE;
        rasterizer_desc.DepthClipEnable = TRUE;
        result = presenter->device->CreateRasterizerState(
            &rasterizer_desc, &presenter->draw_rasterizer_states[mode]);
        if (FAILED(result)) return false;
    }

    /* The guest's own sampler state is a separate seam; linear filtering with
       wrap addressing is the common case for this title's textures. */
    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    result = presenter->device->CreateSamplerState(
        &sampler_desc, &presenter->draw_sampler);
    if (FAILED(result)) {
        return false;
    }
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW =
        D3D11_TEXTURE_ADDRESS_CLAMP;
    result = presenter->device->CreateSamplerState(
        &sampler_desc, &presenter->filter_sampler);
    if (FAILED(result)) return false;

    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW =
        D3D11_TEXTURE_ADDRESS_WRAP;
    result = presenter->device->CreateSamplerState(
        &sampler_desc, &presenter->program_mask_sampler);
    if (FAILED(result)) return false;

    presenter->draw_shared_failed = false;
    presenter->draw_shared_ready = true;
    return true;
}

// Resample only color; depth stays in its existing guest-sized storage.
bool resampleTarget(RecompD3dPresenter *presenter, ID3D11ShaderResourceView *source,
    ID3D11RenderTargetView *target, uint32_t width, uint32_t height)
{
    if (!ensureSharedDrawState(presenter)) return false;
    if (!presenter->target_copy_vs || !presenter->target_copy_ps) {
        constexpr char shader[] =
            "void vs(uint i:SV_VertexID,out float4 p:SV_Position,out float2 uv:TEXCOORD0) {"
            "uv=float2(i==1?2:0,i==2?2:0); p=float4(uv.x*2-1,1-uv.y*2,0,1); }"
            "Texture2D image:register(t0); SamplerState sample_image:register(s0);"
            "float4 ps(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target {"
            "return image.Sample(sample_image,uv); }";
        ID3DBlob *vs = nullptr, *ps = nullptr;
        bool ok = compileDrawShader(shader, "vs", "vs_4_0", &vs) &&
            compileDrawShader(shader, "ps", "ps_4_0", &ps);
        if (ok && !presenter->target_copy_vs) ok = SUCCEEDED(presenter->device->CreateVertexShader(
            vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &presenter->target_copy_vs));
        if (ok && !presenter->target_copy_ps) ok = SUCCEEDED(presenter->device->CreatePixelShader(
            ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &presenter->target_copy_ps));
        releaseCom(vs); releaseCom(ps);
        if (!ok) return false;
    }
    auto *context = presenter->context;
    context->ClearState();
    const D3D11_VIEWPORT viewport = {0,0,float(width),float(height),0,1};
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(presenter->target_copy_vs, nullptr, 0);
    context->PSSetShader(presenter->target_copy_ps, nullptr, 0);
    context->PSSetShaderResources(0, 1, &source);
    context->PSSetSamplers(0, 1, &presenter->filter_sampler);
    context->Draw(3, 0);
    context->ClearState();
    return true;
}

/* Returns the pipeline for one FVF, building it on first use. A pipeline that
   fails to build is remembered against its own FVF so it is not retried every
   draw and does not affect any other FVF. */
const DrawPipeline *lookupDrawPipeline(
    RecompD3dPresenter *presenter,
    uint32_t fvf, const RecompD3dPresenterDrawCommand *draw = nullptr)
{
    const uint32_t count = draw ? draw->program_count : 0u;
    if (count > 136u) return nullptr;
    for (uint32_t i = 0u; i < presenter->draw_pipeline_count; ++i) {
        DrawPipeline &pipeline = presenter->draw_pipelines[i];

        if (pipeline.used && pipeline.fvf == fvf && pipeline.program_count == count &&
            (!count || std::memcmp(pipeline.program, draw->program, count * 16u) == 0)) {
            return pipeline.failed ? nullptr : &pipeline;
        }
    }
    /* The portrait addition pushed distinct FVFs past the fixed slot count,
       which used to reject every later FVF for the whole run. Evict the
       oldest slot FIFO, matching the blend-state cache, and rebuild it. */
    DrawPipeline &pipeline =
        presenter->draw_pipelines[presenter->next_draw_pipeline_slot];
    releaseCom(pipeline.input_layout);
    releaseCom(pipeline.pixel_shader);
    releaseCom(pipeline.vertex_shader);
    presenter->next_draw_pipeline_slot =
        (presenter->next_draw_pipeline_slot + 1u) % kDrawPipelineSlots;
    if (presenter->draw_pipeline_count < kDrawPipelineSlots) {
        ++presenter->draw_pipeline_count;
    }
    pipeline.fvf = fvf;
    pipeline.program_count = count;
    if (count) std::memcpy(pipeline.program, draw->program, count * 16u);
    pipeline.used = true;
    pipeline.failed = !createDrawPipeline(presenter, fvf, pipeline);
    return pipeline.failed ? nullptr : &pipeline;
}

bool ensureDynamicBuffer(
    RecompD3dPresenter *presenter,
    ID3D11Buffer **buffer,
    UINT &capacity,
    UINT required,
    UINT bind_flags)
{
    if (*buffer != nullptr && capacity >= required) {
        return true;
    }

    UINT size = capacity != 0u ? capacity : 4096u;
    while (size < required) {
        size *= 2u;
    }

    releaseCom(*buffer);
    capacity = 0u;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = size;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = bind_flags;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(presenter->device->CreateBuffer(&desc, nullptr, buffer))) {
        return false;
    }
    capacity = size;
    return true;
}

bool uploadBuffer(
    RecompD3dPresenter *presenter,
    ID3D11Buffer *buffer,
    const void *source,
    size_t size)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};

    if (FAILED(presenter->context->Map(
            buffer, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, source, size);
    presenter->context->Unmap(buffer, 0u);
    return true;
}

/* Appends each draw's vertices or indices and discards only when the ring
   wraps. A discard per draw made the driver block for up to ~150 ms per tick
   right after a buffer grew (island map). */
bool uploadRing(
    RecompD3dPresenter *presenter,
    ID3D11Buffer **buffer,
    UINT &capacity,
    UINT &used,
    const void *source,
    UINT size,
    UINT bind_flags,
    UINT &offset)
{
    ID3D11Buffer *const previous = *buffer;
    if (!ensureDynamicBuffer(presenter, buffer, capacity,
            (std::max)(size * 4u, 1u << 20), bind_flags)) {
        return false;
    }
    const bool wrap = *buffer != previous || used + size > capacity;
    if (wrap) used = 0u;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(presenter->context->Map(*buffer, 0u,
            wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0u, &mapped))) {
        return false;
    }
    std::memcpy(static_cast<uint8_t *>(mapped.pData) + used, source, size);
    presenter->context->Unmap(*buffer, 0u);
    offset = used;
    used += (size + 15u) & ~15u;
    return true;
}

D3D11_COMPARISON_FUNC hostCompareFunc(RecompD3dCompareFunc func)
{
    switch (func) {
    case RECOMP_D3D_COMPARE_NEVER:
        return D3D11_COMPARISON_NEVER;
    case RECOMP_D3D_COMPARE_LESS:
        return D3D11_COMPARISON_LESS;
    case RECOMP_D3D_COMPARE_EQUAL:
        return D3D11_COMPARISON_EQUAL;
    case RECOMP_D3D_COMPARE_GREATER:
        return D3D11_COMPARISON_GREATER;
    case RECOMP_D3D_COMPARE_NOT_EQUAL:
        return D3D11_COMPARISON_NOT_EQUAL;
    case RECOMP_D3D_COMPARE_GREATER_EQUAL:
        return D3D11_COMPARISON_GREATER_EQUAL;
    case RECOMP_D3D_COMPARE_ALWAYS:
        return D3D11_COMPARISON_ALWAYS;
    case RECOMP_D3D_COMPARE_LESS_EQUAL:
    default:
        return D3D11_COMPARISON_LESS_EQUAL;
    }
}

D3D11_STENCIL_OP hostStencilOp(RecompD3dStencilOp op)
{
    switch (op) {
    case RECOMP_D3D_STENCIL_ZERO:
        return D3D11_STENCIL_OP_ZERO;
    case RECOMP_D3D_STENCIL_REPLACE:
        return D3D11_STENCIL_OP_REPLACE;
    case RECOMP_D3D_STENCIL_INCRSAT:
        return D3D11_STENCIL_OP_INCR_SAT;
    case RECOMP_D3D_STENCIL_DECRSAT:
        return D3D11_STENCIL_OP_DECR_SAT;
    case RECOMP_D3D_STENCIL_INVERT:
        return D3D11_STENCIL_OP_INVERT;
    case RECOMP_D3D_STENCIL_INCRWRAP:
        return D3D11_STENCIL_OP_INCR;
    case RECOMP_D3D_STENCIL_DECRWRAP:
        return D3D11_STENCIL_OP_DECR;
    case RECOMP_D3D_STENCIL_KEEP:
    default:
        return D3D11_STENCIL_OP_KEEP;
    }
}

ID3D11DepthStencilState *lookupDepthState(
    RecompD3dPresenter *presenter,
    const RecompD3dDepthState &depth)
{
    for (uint32_t i = 0u; i < presenter->depth_state_count; ++i) {
        DepthStateEntry &entry = presenter->depth_states[i];

        if (entry.used && entry.test_enable == depth.depth_test_enable &&
            entry.write_enable == depth.depth_write_enable &&
            entry.func == depth.depth_func &&
            entry.stencil_enable == depth.stencil_enable &&
            entry.stencil_func == depth.stencil_func &&
            entry.stencil_read_mask == depth.stencil_read_mask &&
            entry.stencil_write_mask == depth.stencil_write_mask &&
            entry.stencil_fail == depth.stencil_fail &&
            entry.stencil_zfail == depth.stencil_zfail &&
            entry.stencil_pass == depth.stencil_pass) {
            return entry.state;
        }
    }

    D3D11_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = depth.depth_test_enable ? TRUE : FALSE;
    desc.DepthWriteMask = depth.depth_write_enable
        ? D3D11_DEPTH_WRITE_MASK_ALL
        : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = hostCompareFunc(depth.depth_func);
    desc.StencilEnable = depth.stencil_enable ? TRUE : FALSE;
    desc.StencilReadMask = static_cast<UINT8>(depth.stencil_read_mask);
    desc.StencilWriteMask = static_cast<UINT8>(depth.stencil_write_mask);
    desc.FrontFace.StencilFunc = hostCompareFunc(depth.stencil_func);
    desc.FrontFace.StencilFailOp = hostStencilOp(depth.stencil_fail);
    desc.FrontFace.StencilDepthFailOp = hostStencilOp(depth.stencil_zfail);
    desc.FrontFace.StencilPassOp = hostStencilOp(depth.stencil_pass);
    desc.BackFace = desc.FrontFace;

    ID3D11DepthStencilState *state = nullptr;
    if (FAILED(presenter->device->CreateDepthStencilState(&desc, &state))) {
        return nullptr;
    }

    // ponytail: FIFO eviction; track recent use if state creation churn is costly.
    DepthStateEntry &entry =
        presenter->depth_states[presenter->next_depth_state_slot];
    releaseCom(entry.state);
    presenter->next_depth_state_slot =
        (presenter->next_depth_state_slot + 1u) % kDepthStateSlots;
    if (presenter->depth_state_count < kDepthStateSlots) {
        ++presenter->depth_state_count;
    }
    entry.used = true;
    entry.test_enable = depth.depth_test_enable;
    entry.write_enable = depth.depth_write_enable;
    entry.func = depth.depth_func;
    entry.stencil_enable = depth.stencil_enable;
    entry.stencil_func = depth.stencil_func;
    entry.stencil_read_mask = depth.stencil_read_mask;
    entry.stencil_write_mask = depth.stencil_write_mask;
    entry.stencil_fail = depth.stencil_fail;
    entry.stencil_zfail = depth.stencil_zfail;
    entry.stencil_pass = depth.stencil_pass;
    entry.state = state;
    static unsigned depth_state_lines;
    if (depth_state_lines < kDepthStateSlots) {
        ++depth_state_lines;
        std::fprintf(
            stderr,
            "recomp d3d presenter: depth state test=%d write=%d func=%d stencil=%d\n",
            depth.depth_test_enable ? 1 : 0,
            depth.depth_write_enable ? 1 : 0,
            static_cast<int>(depth.depth_func),
            depth.stencil_enable ? 1 : 0);
    }
    return state;
}

D3D11_BLEND hostBlendFactor(RecompD3dBlendFactor factor, bool alpha_channel)
{
    switch (factor) {
    case RECOMP_D3D_BLEND_ZERO:
        return D3D11_BLEND_ZERO;
    case RECOMP_D3D_BLEND_CONSTANT_COLOR:
        return D3D11_BLEND_BLEND_FACTOR;
    case RECOMP_D3D_BLEND_INV_CONSTANT_COLOR:
        return D3D11_BLEND_INV_BLEND_FACTOR;
    case RECOMP_D3D_BLEND_SRC_COLOR:
        /* The alpha blend equation may only name alpha operands. */
        return alpha_channel ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case RECOMP_D3D_BLEND_INV_SRC_COLOR:
        return alpha_channel ? D3D11_BLEND_INV_SRC_ALPHA
                             : D3D11_BLEND_INV_SRC_COLOR;
    case RECOMP_D3D_BLEND_SRC_ALPHA:
        return D3D11_BLEND_SRC_ALPHA;
    case RECOMP_D3D_BLEND_INV_SRC_ALPHA:
        return D3D11_BLEND_INV_SRC_ALPHA;
    case RECOMP_D3D_BLEND_DST_ALPHA:
        return D3D11_BLEND_DEST_ALPHA;
    case RECOMP_D3D_BLEND_INV_DST_ALPHA:
        return D3D11_BLEND_INV_DEST_ALPHA;
    case RECOMP_D3D_BLEND_DST_COLOR:
        return alpha_channel ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case RECOMP_D3D_BLEND_INV_DST_COLOR:
        return alpha_channel ? D3D11_BLEND_INV_DEST_ALPHA
                             : D3D11_BLEND_INV_DEST_COLOR;
    case RECOMP_D3D_BLEND_SRC_ALPHA_SATURATE:
        return D3D11_BLEND_SRC_ALPHA_SAT;
    case RECOMP_D3D_BLEND_ONE:
    default:
        return D3D11_BLEND_ONE;
    }
}

D3D11_BLEND_OP hostBlendOp(RecompD3dBlendOp op)
{
    switch (op) {
    case RECOMP_D3D_BLEND_OP_MIN:
        return D3D11_BLEND_OP_MIN;
    case RECOMP_D3D_BLEND_OP_MAX:
        return D3D11_BLEND_OP_MAX;
    case RECOMP_D3D_BLEND_OP_SUBTRACT:
        return D3D11_BLEND_OP_SUBTRACT;
    case RECOMP_D3D_BLEND_OP_REVERSE_SUBTRACT:
        return D3D11_BLEND_OP_REV_SUBTRACT;
    case RECOMP_D3D_BLEND_OP_ADD:
    default:
        return D3D11_BLEND_OP_ADD;
    }
}

/* Xbox block-compressed formats map straight onto the host equivalents, and
   both store 4x4 blocks in the same order, so the guest bytes upload as-is.
   The uncompressed formats need unswizzling first. */
DXGI_FORMAT hostTextureFormat(uint32_t format_byte)
{
    switch (format_byte) {
    case RECOMP_D3D_TEXTURE_FORMAT_DXT1:
        return DXGI_FORMAT_BC1_UNORM;
    case RECOMP_D3D_TEXTURE_FORMAT_DXT3:
        return DXGI_FORMAT_BC2_UNORM;
    case RECOMP_D3D_TEXTURE_FORMAT_DXT5:
        return DXGI_FORMAT_BC3_UNORM;
    case RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8:
    case RECOMP_D3D_TEXTURE_FORMAT_P8:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case RECOMP_D3D_TEXTURE_FORMAT_A8:
        return DXGI_FORMAT_A8_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool isSwizzledTextureFormat(uint32_t format_byte)
{
    return format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8 ||
        format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8;
}

/* Draw-time tally of the formats a draw actually sampled, keyed by format
   byte, plus how many draws wanted a texture and got none. */
struct DrawTextureTally {
    uint32_t textured[256];
    uint32_t rejected[256];
    uint32_t untextured;
};

DrawTextureTally draw_texture_tally;

void recompD3dPresenterCountDrawTexture(
    const RecompD3dPresenterDrawCommand &draw,
    bool sampled)
{
    if (!draw.has_texture) {
        ++draw_texture_tally.untextured;
        return;
    }
    if (sampled) {
        ++draw_texture_tally.textured[draw.texture.format_byte & 0xffu];
    } else {
        ++draw_texture_tally.rejected[draw.texture.format_byte & 0xffu];
    }
}

/* Opt-in, one pair of water input readbacks after the draw-capture trigger. */
void dumpProgramTexture(RecompD3dPresenter *presenter,
    ID3D11ShaderResourceView *view, unsigned index)
{
    static const char *prefix = std::getenv("RECOMP_D3D_PROGRAM_TEXTURE_DUMP");
    static const char *trigger = std::getenv("RECOMP_D3D_DRAW_CAPTURE_TRIGGER");
    static unsigned captured = 0;
    const unsigned bit = 1u << index;
    if (!prefix || !trigger || (captured & bit) ||
        GetFileAttributesA(trigger) == INVALID_FILE_ATTRIBUTES || !view) return;
    captured |= bit;
    ID3D11Resource *resource = nullptr;
    ID3D11Texture2D *texture = nullptr, *staging = nullptr;
    view->GetResource(&resource);
    HRESULT result = resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void **>(&texture));
    releaseCom(resource);
    if (FAILED(result)) return;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
        desc.Width <= 720 && desc.Height <= 512 && desc.MipLevels == 1) {
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        result = presenter->device->CreateTexture2D(&desc, nullptr, &staging);
        if (SUCCEEDED(result)) {
            presenter->context->CopyResource(staging, texture);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            result = presenter->context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(result)) {
                char path[1024];
                std::snprintf(path, sizeof path, "%s.%u.%ux%u.bgra", prefix,
                    index, desc.Width, desc.Height);
                if (FILE *file = std::fopen(path, "wb")) {
                    for (unsigned y = 0; y < desc.Height; ++y)
                        std::fwrite(static_cast<const unsigned char *>(mapped.pData) +
                            size_t(y) * mapped.RowPitch, 4, desc.Width, file);
                    std::fclose(file);
                    std::fprintf(stderr, "recomp program input: %s present=%u\n",
                        path, presenter->present_count);
                }
                presenter->context->Unmap(staging, 0);
            }
        }
    }
    releaseCom(staging);
    releaseCom(texture);
}

void copyGuestBuffer(RecompD3dPresenter *presenter, ID3D11Resource *target)
{
    ID3D11Resource *source = nullptr;
    presenter->render_target_view->GetResource(&source);
    if (presenter->msaa > 1u) presenter->context->ResolveSubresource(
        target, 0u, source, 0u, DXGI_FORMAT_B8G8R8A8_UNORM);
    else presenter->context->CopySubresourceRegion(target, 0u, 0u, 0u, 0u, source, 0u, nullptr);
    releaseCom(source);
}

bool createBufferCopy(RecompD3dPresenter *presenter,
    ID3D11Texture2D *&copy, ID3D11ShaderResourceView *&sample, bool mips = false)
{
    if (copy != nullptr) return true;
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = mainWidth(presenter);
    texture_desc.Height = mainHeight(presenter);
    // Upscaled back-buffer snapshots need mips for guest downsampling.
    texture_desc.MipLevels = mips ? 0u : 1u;
    texture_desc.ArraySize = 1u;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.SampleDesc.Count = 1u;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
        (mips ? D3D11_BIND_RENDER_TARGET : 0u);
    texture_desc.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0u;
    HRESULT result = presenter->device->CreateTexture2D(&texture_desc, nullptr, &copy);
    if (SUCCEEDED(result)) {
        result = presenter->device->CreateShaderResourceView(copy, nullptr, &sample);
    }
    if (FAILED(result)) {
        releaseCom(sample);
        releaseCom(copy);
        return false;
    }
    return true;
}

// Keep the presented frame for draws that sample the guest front buffer.
void copyFrontBuffer(RecompD3dPresenter *presenter)
{
    if (createBufferCopy(presenter, presenter->front_buffer_copy,
            presenter->front_buffer_sample)) {
        if (presenter->gamma_enabled || presenter->smaa) {
            presenter->context->CopySubresourceRegion(
                presenter->front_buffer_copy, 0u, 0u, 0u, 0u,
                presenter->back_buffer_copy, 0u, nullptr);
        } else {
            copyGuestBuffer(presenter, presenter->front_buffer_copy);
        }
    }
}

ID3D11ShaderResourceView *lookupBackBufferTexture(
    RecompD3dPresenter *presenter,
    const RecompD3dTextureDesc &desc)
{
    /* 3D scenes use a horizontally supersampled guest back buffer (1440x480
       for 720x480 output). The host keeps one resolved buffer, and linear
       UVs are normalised by the guest size, so whole multiples map onto it. */
    if (desc.format_byte != 0x12u || !desc.linear || desc.depth ||
        desc.width == 0u || desc.height == 0u ||
        desc.width % presenter->config.width != 0u ||
        desc.height % presenter->config.height != 0u ||
        presenter->render_target_view == nullptr) return nullptr;

    if (!createBufferCopy(presenter, presenter->back_buffer_copy,
            presenter->back_buffer_sample, presenter->scale != 1.0f)) return nullptr;
    /* Sampling observes the current render buffer at this draw, even if the
       preceding draw sampled an older copy. Keep the copy outside the FIFO. */
    ID3D11ShaderResourceView *none = nullptr;
    presenter->context->PSSetShaderResources(0u, 1u, &none);
    copyGuestBuffer(presenter, presenter->back_buffer_copy);
    /* Every view exposes the chain; refresh it with each new snapshot. */
    if (presenter->scale != 1.0f)
        presenter->context->GenerateMips(presenter->back_buffer_sample);
    return presenter->back_buffer_sample;
}

/* Xbox D3DTADDRESS WRAP..BORDER (1..4) share D3D11's values and CLAMPTOEDGE
   clamps. The casino Zack sprite draws V 0..2 with CLAMP; wrap shows it twice. */
ID3D11SamplerState *lookupDrawSampler(
    RecompD3dPresenter *presenter, uint32_t address_u, uint32_t address_v)
{
    const auto host = [](uint32_t mode) {
        return mode == 5u ? D3D11_TEXTURE_ADDRESS_CLAMP : mode >= 1u && mode <= 4u
            ? static_cast<D3D11_TEXTURE_ADDRESS_MODE>(mode) : D3D11_TEXTURE_ADDRESS_WRAP;
    };
    if (presenter->draw_sampler == nullptr) return nullptr;
    const D3D11_TEXTURE_ADDRESS_MODE u = host(address_u), v = host(address_v);
    ID3D11SamplerState *&sampler = presenter->address_samplers[u - 1][v - 1];
    if (sampler == nullptr) {
        D3D11_SAMPLER_DESC desc{};
        presenter->draw_sampler->GetDesc(&desc);
        desc.AddressU = u;
        desc.AddressV = v;
        if (FAILED(presenter->device->CreateSamplerState(&desc, &sampler))) {
            return presenter->draw_sampler;
        }
    }
    return sampler;
}

void unindexTexture(RecompD3dPresenter *presenter, uint32_t slot)
{
    const auto range = presenter->texture_index.equal_range(presenter->textures[slot].data);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == slot) {
            presenter->texture_index.erase(it);
            return;
        }
    }
}


ID3D11ShaderResourceView *lookupTexture(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterDrawCommand &draw)
{
    const RecompD3dTextureDesc &desc = draw.texture;
    const bool palettized = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_P8;
    const bool linear_bgra = desc.format_byte == 0x12u;
    const bool compressed = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT1 ||
        desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT3 || desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT5;
    const uint32_t levels = compressed && desc.mip_levels ? desc.mip_levels : 1u;
    if (compressed) {
        const uint32_t span = recomp_d3d_texture_compressed_mip_span(&desc);
        if (span == 0u || span > draw.texture_byte_count ||
            desc.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            desc.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) return nullptr;
    }

    if (draw.texture_is_backbuffer) return lookupBackBufferTexture(presenter, desc);
    if (draw.texture_is_frontbuffer) return presenter->front_buffer_sample;

    if (palettized && (draw.palette_bytes == nullptr ||
        draw.palette_byte_count != kPaletteBytes)) {
        return nullptr;
    }

    if (RenderTargetEntry *entry = findRenderTarget(presenter, desc)) {
        return entry->sample_view;
    }
    if (linear_bgra && (!desc.linear || desc.depth ||
        desc.bits_per_pixel != 32u || desc.width == 0u || desc.height == 0u ||
        desc.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        desc.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        desc.pitch < desc.width * 4u || draw.texture_bytes == nullptr ||
        static_cast<uint64_t>(desc.height - 1u) * desc.pitch + desc.width * 4u >
            draw.texture_byte_count)) {
        return nullptr;
    }
    const auto cached = presenter->texture_index.equal_range(desc.data);
    const bool fingerprinted = !linear_bgra && draw.texture_bytes != nullptr;
    const uint64_t fingerprint = fingerprinted
        ? textureFingerprint(draw.texture_bytes, draw.texture_byte_count) : 0u;
    for (auto it = cached.first; it != cached.second; ++it) {
        TextureEntry &entry = presenter->textures[it->second];

        if (entry.format_byte == desc.format_byte &&
            entry.width == desc.width && entry.height == desc.height && entry.mip_levels == levels &&
            (!palettized || std::memcmp(entry.palette, draw.palette_bytes, kPaletteBytes) == 0)) {
            if (fingerprinted && entry.fingerprint != fingerprint) {
                static unsigned reported;
                if (reported < 32u) {
                    ++reported;
                    std::fprintf(stderr, "recomp d3d presenter: texels changed data=0x%08X "
                        "fmt=0x%02X size=%ux%u\n", desc.data, desc.format_byte, desc.width, desc.height);
                }
                unindexTexture(presenter, it->second);
                releaseCom(entry.view);
                entry.used = false;
                break;
            }
            if (linear_bgra) {
                ID3D11Resource *resource = nullptr;
                entry.view->GetResource(&resource);
                presenter->context->UpdateSubresource(
                    resource, 0u, nullptr, draw.texture_bytes, desc.pitch, 0u);
                releaseCom(resource);
            }
            return entry.view;
        }
    }
    const DXGI_FORMAT format = linear_bgra
        ? DXGI_FORMAT_B8G8R8A8_UNORM : hostTextureFormat(desc.format_byte);
    if (format == DXGI_FORMAT_UNKNOWN || draw.texture_bytes == nullptr ||
        draw.texture_byte_count == 0u || (desc.linear && !linear_bgra)) {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = desc.width;
    texture_desc.Height = desc.height;
    texture_desc.MipLevels = levels;
    texture_desc.ArraySize = 1u;
    texture_desc.Format = format;
    texture_desc.SampleDesc.Count = 1u;
    texture_desc.Usage = linear_bgra ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::vector<D3D11_SUBRESOURCE_DATA> subresources(levels);
    D3D11_SUBRESOURCE_DATA &initial = subresources[0];
    std::vector<uint8_t> unswizzled;

    if (linear_bgra) {
        initial.pSysMem = draw.texture_bytes;
        initial.SysMemPitch = desc.pitch;
    } else if (palettized) {
        const size_t texels = static_cast<size_t>(desc.width) * desc.height;
        if (desc.width == 0u || desc.height == 0u ||
            desc.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            desc.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            texels > draw.texture_byte_count) {
            return nullptr;
        }
        std::vector<uint8_t> indices(texels);
        if (!recomp_d3d_texture_unswizzle(
                static_cast<const uint8_t *>(draw.texture_bytes),
                indices.data(), desc.width, desc.height, 1u)) {
            return nullptr;
        }
        unswizzled.resize(texels * 4u);
        const auto *palette = static_cast<const uint8_t *>(draw.palette_bytes);
        for (size_t i = 0u; i < texels; ++i) {
            /* Little-endian ARGB palette words are already BGRA bytes. */
            std::memcpy(unswizzled.data() + i * 4u, palette + indices[i] * 4u, 4u);
        }
        initial.pSysMem = unswizzled.data();
        initial.SysMemPitch = desc.width * 4u;
    } else if (isSwizzledTextureFormat(desc.format_byte)) {
        const uint32_t texel_bytes = desc.bits_per_pixel / 8u;

        unswizzled.resize(
            static_cast<size_t>(desc.width) * desc.height * texel_bytes);
        if (!recomp_d3d_texture_unswizzle(
                static_cast<const uint8_t *>(draw.texture_bytes),
                unswizzled.data(),
                desc.width,
                desc.height,
                texel_bytes)) {
            return nullptr;
        }
        initial.pSysMem = unswizzled.data();
        initial.SysMemPitch = desc.width * texel_bytes;
    } else {
        const uint32_t block_bytes = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT1 ? 8u : 16u;
        uint32_t width = desc.width, height = desc.height, offset = 0u;
        for (uint32_t level = 0; level < levels; ++level) {
            subresources[level].pSysMem = static_cast<const uint8_t *>(draw.texture_bytes) + offset;
            subresources[level].SysMemPitch = ((width + 3u) / 4u) * block_bytes;
            offset += subresources[level].SysMemPitch * ((height + 3u) / 4u);
            width = width > 1u ? width / 2u : 1u;
            height = height > 1u ? height / 2u : 1u;
        }
    }

    ID3D11Texture2D *texture = nullptr;
    if (FAILED(presenter->device->CreateTexture2D(
            &texture_desc, subresources.data(), &texture))) {
        return nullptr;
    }

    ID3D11ShaderResourceView *view = nullptr;
    const HRESULT view_result =
        presenter->device->CreateShaderResourceView(texture, nullptr, &view);
    releaseCom(texture);
    if (FAILED(view_result)) {
        return nullptr;
    }

    // ponytail: FIFO eviction; track recent use if upload churn is costly.
    const uint32_t slot = presenter->next_texture_slot;
    TextureEntry &entry = presenter->textures[slot];
    if (entry.used) unindexTexture(presenter, slot);
    releaseCom(entry.view);
    presenter->next_texture_slot =
        (presenter->next_texture_slot + 1u) % kTextureSlots;
    if (presenter->texture_count < kTextureSlots) {
        ++presenter->texture_count;
    }
    entry.used = true;
    entry.data = desc.data;
    entry.format_byte = desc.format_byte;
    entry.width = desc.width;
    entry.height = desc.height;
    entry.mip_levels = levels;
    if (palettized) std::memcpy(entry.palette, draw.palette_bytes, kPaletteBytes);
    entry.fingerprint = fingerprint;
    entry.view = view;
    try {
        presenter->texture_index.emplace(desc.data, slot);
    } catch (const std::bad_alloc &) {
        releaseCom(entry.view);
        entry.used = false;
        return nullptr;
    }
    return view;
}

ID3D11BlendState *lookupBlendState(
    RecompD3dPresenter *presenter,
    const RecompD3dBlendState &blend)
{
    for (uint32_t i = 0u; i < presenter->blend_state_count; ++i) {
        BlendStateEntry &entry = presenter->blend_states[i];

        if (entry.used && entry.enable == blend.blend_enable &&
            entry.src == blend.src_factor && entry.dst == blend.dst_factor &&
            entry.op == blend.op &&
            entry.color_write_mask == blend.color_write_mask) {
            return entry.state;
        }
    }

    D3D11_BLEND_DESC desc{};
    D3D11_RENDER_TARGET_BLEND_DESC &target = desc.RenderTarget[0];
    target.BlendEnable = blend.blend_enable ? TRUE : FALSE;
    target.SrcBlend = hostBlendFactor(blend.src_factor, false);
    target.DestBlend = hostBlendFactor(blend.dst_factor, false);
    target.BlendOp = hostBlendOp(blend.op);
    target.SrcBlendAlpha = hostBlendFactor(blend.src_factor, true);
    target.DestBlendAlpha = hostBlendFactor(blend.dst_factor, true);
    target.BlendOpAlpha = hostBlendOp(blend.op);
    target.RenderTargetWriteMask = blend.color_write_mask;

    ID3D11BlendState *state = nullptr;
    if (FAILED(presenter->device->CreateBlendState(&desc, &state))) {
        return nullptr;
    }

    // ponytail: FIFO eviction; track recent use if state creation churn is costly.
    BlendStateEntry &entry =
        presenter->blend_states[presenter->next_blend_state_slot];
    releaseCom(entry.state);
    presenter->next_blend_state_slot =
        (presenter->next_blend_state_slot + 1u) % kBlendStateSlots;
    if (presenter->blend_state_count < kBlendStateSlots) {
        ++presenter->blend_state_count;
    }
    entry.used = true;
    entry.enable = blend.blend_enable;
    entry.src = blend.src_factor;
    entry.dst = blend.dst_factor;
    entry.op = blend.op;
    entry.color_write_mask = blend.color_write_mask;
    entry.state = state;
    static unsigned blend_state_lines;
    if (blend_state_lines < kBlendStateSlots) {
        ++blend_state_lines;
        std::fprintf(
            stderr,
            "recomp d3d presenter: blend state enable=%d src=%d dst=%d op=%d mask=0x%02X\n",
            blend.blend_enable ? 1 : 0,
            static_cast<int>(blend.src_factor),
            static_cast<int>(blend.dst_factor),
            static_cast<int>(blend.op),
            static_cast<unsigned>(blend.color_write_mask));
    }
    return state;
}

RecompD3dPresenterError submitDraw(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterDrawCommand &draw)
{
    if (draw.vertex_bytes == nullptr || draw.index_bytes == nullptr ||
        draw.vertex_stride == 0u || draw.vertex_count == 0u ||
        draw.index_count == 0u ||
        static_cast<unsigned>(draw.cull_mode) > RECOMP_D3D_CULL_COUNTER_CLOCKWISE) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }

    RecompD3dVertexLayout layout;
    if (!recomp_d3d_fvf_layout(draw.fvf, &layout) ||
        layout.stride > draw.vertex_stride ||
        (!layout.pretransformed && !draw.has_transform) ||
        (draw.blend_weight_count != 0u &&
         layout.blend_weight_count != draw.blend_weight_count)) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }

    D3D11_PRIMITIVE_TOPOLOGY topology;
    switch (draw.primitive_type) {
    case 5u:
        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
    case 6u:
        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        break;
    default:
        /* D3D11 has no triangle fan. Fans are converted to a list by the
           caller-side index expansion below. */
        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
    }

    if (!ensureSharedDrawState(presenter)) {
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }

    const DrawPipeline *pipeline = lookupDrawPipeline(presenter, draw.fvf, &draw);
    if (pipeline == nullptr) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }

    ID3D11RenderTargetView *color_view;
    ID3D11DepthStencilView *depth_view;
    const RecompD3dPresenterError target_result =
        bindTarget(presenter, draw.target, color_view, depth_view,
            draw.texture_is_frontbuffer);
    if (target_result != RECOMP_D3D_PRESENTER_OK) {
        return target_result;
    }
    if (draw.has_texture) {
        const RenderTargetEntry *sampled = findRenderTarget(presenter, draw.texture);
        if (sampled != nullptr && sampled->render_view == color_view) {
            std::fprintf(stderr,
                "recomp d3d presenter: cannot sample active render target "
                "data=0x%08X\n", draw.texture.data);
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }
    }

    const UINT vertex_size = draw.vertex_stride * draw.vertex_count;
    UINT vertex_offset = 0u;
    if (!uploadRing(presenter, &presenter->draw_vertex_buffer,
            presenter->draw_vertex_capacity, presenter->draw_vertex_used,
            draw.vertex_bytes, vertex_size, D3D11_BIND_VERTEX_BUFFER, vertex_offset)) {
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }

    /* Triangle fans are expanded into a triangle list here because D3D11
       dropped fan topology; every other supported primitive passes through
       with its indices unchanged. */
    const auto *source_indices =
        static_cast<const uint16_t *>(draw.index_bytes);
    UINT draw_index_count = draw.index_count;
    static uint16_t fan_indices[3u * 4096u];
    const void *upload_indices = draw.index_bytes;

    if (draw.primitive_type == 7u) {
        if (draw.triangle_count > 4096u) {
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }
        for (uint32_t i = 0u; i < draw.triangle_count; ++i) {
            fan_indices[i * 3u + 0u] = source_indices[0];
            fan_indices[i * 3u + 1u] = source_indices[i + 1u];
            fan_indices[i * 3u + 2u] = source_indices[i + 2u];
        }
        draw_index_count = draw.triangle_count * 3u;
        upload_indices = fan_indices;
    }

    const UINT index_size = draw_index_count * 2u;
    ID3D11ShaderResourceView *texture_view =
        draw.has_texture ? lookupTexture(presenter, draw) : nullptr;
    if ((draw.texture_is_frontbuffer || draw.texture_is_backbuffer) && texture_view == nullptr)
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    /* A second cache lookup can evict the first entry. Keep its view alive
       until the context takes its own reference. */
    const bool has_second_texture = draw.has_alpha_mask || draw.has_reflection || draw.program_alpha_mask;
    if (has_second_texture && texture_view != nullptr) texture_view->AddRef();
    const auto release_view = [](ID3D11ShaderResourceView *view) { if (view) view->Release(); };
    std::unique_ptr<ID3D11ShaderResourceView, decltype(release_view)> retained(
        has_second_texture ? texture_view : nullptr, release_view);
    ID3D11ShaderResourceView *mask_view = nullptr;
    if (draw.has_alpha_mask) {
        if (layout.texcoord_count != 2u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        RecompD3dPresenterDrawCommand mask{};
        mask.texture = draw.alpha_mask;
        mask.texture_bytes = draw.alpha_mask_bytes;
        mask.texture_byte_count = draw.alpha_mask_byte_count;
        mask.palette_bytes = draw.alpha_mask_palette;
        mask.palette_byte_count = draw.alpha_mask_palette_byte_count;
        mask_view = lookupTexture(presenter, mask);
        if (mask_view == nullptr || texture_view == nullptr) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.has_reflection || draw.program_alpha_mask) {
        if (draw.has_alpha_mask || draw.four_tap_filter || layout.pretransformed ||
            (draw.program_alpha_mask && layout.normal_offset == RECOMP_D3D_FVF_ABSENT) ||
            layout.texcoord_count == 0u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        RecompD3dPresenterDrawCommand reflection{};
        reflection.texture = draw.reflection_texture;
        reflection.texture_bytes = draw.reflection_bytes;
        reflection.texture_byte_count = draw.reflection_byte_count;
        mask_view = lookupTexture(presenter, reflection);
        if (mask_view == nullptr || texture_view == nullptr) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.four_tap_filter && texture_view == nullptr) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.program_count) dumpProgramTexture(presenter, texture_view, draw.program_alpha_mask);
    /* Observation only: bind counts say what the guest selected, not what a
       draw actually consumed, and only the latter can explain the frame. */
    recompD3dPresenterCountDrawTexture(draw, texture_view != nullptr);
    float draw_constants[140 + 192 * 4 + 300 + 76]{};
    std::memcpy(draw_constants, draw.transform, sizeof draw.transform);
    std::memcpy(draw_constants + 16, draw.blend_transforms, sizeof draw.blend_transforms);
    if (layout.pretransformed || draw.program_count) {
        D3D11_VIEWPORT viewport{};
        UINT count = 1u;
        presenter->context->RSGetViewports(&count, &viewport);
        /* Guest pixel coordinates stay in guest units on a scaled target. */
        viewport.TopLeftX /= presenter->target_scale_x;
        viewport.TopLeftY /= presenter->target_scale_y;
        viewport.Width /= presenter->target_scale_x;
        viewport.Height /= presenter->target_scale_y;
        if (count != 1u || viewport.Width <= 0.0f || viewport.Height <= 0.0f ||
            viewport.MaxDepth <= viewport.MinDepth) {
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }
        /* Undo the bound viewport; legacy pixel centers are integers, while
           D3D11 centers are half integers. The shader restores clip W from RHW. */
        std::memset(draw_constants, 0, sizeof draw.transform);
        draw_constants[0] = 2.0f / viewport.Width;
        draw_constants[5] = -2.0f / viewport.Height;
        draw_constants[10] = 1.0f / (viewport.MaxDepth - viewport.MinDepth);
        draw_constants[12] = -1.0f + (0.5f - viewport.TopLeftX) * draw_constants[0];
        draw_constants[13] = 1.0f + (0.5f - viewport.TopLeftY) * draw_constants[5];
        draw_constants[14] = -viewport.MinDepth * draw_constants[10];
        draw_constants[15] = 1.0f;
    }
    if (draw.program_count) {
        std::memcpy(draw_constants + 140, draw.program_constants, sizeof draw.program_constants);
        /* Undo the guest sample grid, which may differ from the host target. */
        for (unsigned axis = 0; axis < 2; ++axis) {
            const float scale = draw.program_constants[58][axis];
            if (!std::isfinite(scale) || scale == 0) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
            draw_constants[axis * 5] = 1.0f / scale;
            draw_constants[12 + axis] = -draw.program_constants[59][axis] / scale;
        }
        const float depth_scale = draw.program_constants[58][2];
        if (!std::isfinite(depth_scale) || depth_scale <= 0) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        draw_constants[10] = 1.0f / depth_scale;
        draw_constants[14] = -draw.program_constants[59][2] / depth_scale;
        draw_constants[138] = draw.program_alpha_mask ? 1.0f : 0.0f;
        draw_constants[139] = draw.program_mask_lod_bias;
    }
    if (draw.directional.enabled && !draw.program_count) {
        const auto &light = draw.directional;
        if (light.count > 8u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        float *constants = draw_constants + 140 + 192 * 4;
        std::memcpy(constants, light.normal_transforms, sizeof light.normal_transforms);
        std::memcpy(constants+64, light.ambient_emissive, sizeof light.ambient_emissive);
        std::memcpy(constants+68, light.material_diffuse, sizeof light.material_diffuse);
        std::memcpy(constants+72, light.directions, sizeof light.directions);
        std::memcpy(constants+104, light.colors, sizeof light.colors);
        constants[136]=1;
        constants[137]=light.normalize ? 1.0f:0.0f;
        constants[138]=static_cast<float>(light.count);
        std::memcpy(constants+140, light.world_transforms, sizeof light.world_transforms);
        std::memcpy(constants+204, light.positions, sizeof light.positions);
        std::memcpy(constants+236, light.attenuation, sizeof light.attenuation);
        std::memcpy(constants+268, light.ambient, sizeof light.ambient);
    }
    {
        static const bool fog_enabled = [] {
            const char *value = std::getenv("RECOMP_D3D_FOG");
            return !value || std::strcmp(value, "0") != 0;
        }();
        static const bool fog_visualize = [] {
            const char *value = std::getenv("RECOMP_D3D_FOG_FACTOR");
            return value && std::strcmp(value, "1") == 0;
        }();
        float *fog = draw_constants + 140 + 192 * 4 + 300;
        for (unsigned c = 0; c < 3; ++c)
            fog[c] = ((draw.fog.color >> (16 - 8*c)) & 255u) / 255.0f;
        fog[4] = draw.fog.start;
        fog[5] = draw.fog.end;
        fog[6] = draw.fog.density;
        fog[7] = static_cast<float>(draw.fog.mode);
        fog[8] = draw.fog.enabled && fog_enabled && !draw.program_count ? 1.0f : 0.0f;
        fog[9] = draw.fog_z ? 1.0f : 0.0f;
        fog[10] = draw.fog.range && !layout.pretransformed ? 1.0f : 0.0f;
        fog[11] = fog_visualize ? 1.0f : 0.0f;
        std::memcpy(fog + 12, draw.fog_world_view, sizeof draw.fog_world_view);
    }
    draw_constants[64] = texture_view != nullptr ? 1.0f : 0.0f;
    draw_constants[65] = draw.depth.alpha_test_enable ? 1.0f : 0.0f;
    draw_constants[66] = static_cast<float>(draw.depth.alpha_func);
    draw_constants[67] = static_cast<float>(draw.depth.alpha_ref);
    draw_constants[68] = draw.blend_weight_count != 0u ? 1.0f : 0.0f;
    draw_constants[69] = draw.use_texture_factor ? 1.0f
        : (draw.modulate_texture_factor ? 2.0f : 0.0f);
    draw_constants[70] = draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_NONE
        ? 1.0f : draw.material_alpha;
    draw_constants[71] = draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE
        ? 1.0f : 0.0f;
    draw_constants[72] = ((draw.texture_factor >> 16u) & 0xffu) / 255.0f;
    draw_constants[73] = ((draw.texture_factor >> 8u) & 0xffu) / 255.0f;
    draw_constants[74] = (draw.texture_factor & 0xffu) / 255.0f;
    draw_constants[75] = ((draw.texture_factor >> 24u) & 0xffu) / 255.0f;
    draw_constants[76] = draw.zero_diffuse_rgb ? 1.0f : 0.0f;
    /* NV2A A8 supplies white RGB; DXGI A8 supplies only alpha. */
    draw_constants[77] = draw.texture.format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8
        ? 1.0f : 0.0f;
    draw_constants[80] = draw.texture.linear && draw.texture.width != 0u
        ? 1.0f / draw.texture.width : 1.0f;
    draw_constants[81] = draw.texture.linear && draw.texture.height != 0u
        ? 1.0f / draw.texture.height : 1.0f;
    draw_constants[82] = draw.four_tap_filter ? 1.0f : 0.0f;
    draw_constants[78] = draw.alpha_mask.linear && draw.alpha_mask.width
        ? 1.0f / draw.alpha_mask.width : 1.0f;
    draw_constants[79] = draw.alpha_mask.linear && draw.alpha_mask.height
        ? 1.0f / draw.alpha_mask.height : 1.0f;
    draw_constants[83] = draw.has_alpha_mask ? 1.0f : 0.0f;
    if (draw.has_reflection) {
        std::memcpy(draw_constants + 84, draw.reflection_world_view, 64u);
        std::memcpy(draw_constants + 100, draw.reflection_normal, 64u);
        std::memcpy(draw_constants + 116, draw.reflection_transform, 64u);
        std::memcpy(draw_constants + 132, draw.reflection_diffuse, 16u);
        draw_constants[136] = draw.reflection_mesh_uv ? 2.0f : 1.0f;
        draw_constants[137] = draw.reflection_normalize ? 1.0f : 0.0f;
    }

    UINT index_offset = 0u;
    if (!uploadRing(presenter, &presenter->draw_index_buffer,
            presenter->draw_index_capacity, presenter->draw_index_used,
            upload_indices, index_size, D3D11_BIND_INDEX_BUFFER, index_offset) ||
        !uploadBuffer(
            presenter,
            presenter->draw_constant_buffer,
            draw_constants,
            sizeof draw_constants)) {
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }

    const UINT stride = draw.vertex_stride;
    presenter->context->IASetInputLayout(pipeline->input_layout);
    presenter->context->IASetVertexBuffers(
        0u, 1u, &presenter->draw_vertex_buffer, &stride, &vertex_offset);
    presenter->context->IASetIndexBuffer(
        presenter->draw_index_buffer, DXGI_FORMAT_R16_UINT, index_offset);
    presenter->context->IASetPrimitiveTopology(topology);
    presenter->context->VSSetShader(pipeline->vertex_shader, nullptr, 0u);
    presenter->context->VSSetConstantBuffers(
        0u, 1u, &presenter->draw_constant_buffer);
    presenter->context->PSSetShader(pipeline->pixel_shader, nullptr, 0u);
    presenter->context->PSSetConstantBuffers(
        0u, 1u, &presenter->draw_constant_buffer);
    ID3D11ShaderResourceView *views[] = {texture_view, mask_view};
    presenter->context->PSSetShaderResources(0u, 2u, views);
    ID3D11SamplerState *samplers[] = {
        draw.four_tap_filter || draw.has_alpha_mask || draw.program_count
            ? presenter->filter_sampler
            : lookupDrawSampler(presenter, draw.address_u, draw.address_v),
        draw.program_alpha_mask ? presenter->program_mask_sampler : presenter->filter_sampler};
    presenter->context->PSSetSamplers(0u, 2u, samplers);
    presenter->context->RSSetState(presenter->draw_rasterizer_states[draw.cull_mode]);
    ID3D11DepthStencilState *depth_state = lookupDepthState(presenter, draw.depth);
    if (depth_state == nullptr) {
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    presenter->context->OMSetDepthStencilState(depth_state, draw.depth.stencil_ref);
    {
        const uint32_t color = draw.blend.constant_color;
        const float blend_factor[4] = {
            ((color >> 16u) & 255u) / 255.0f,
            ((color >> 8u) & 255u) / 255.0f,
            (color & 255u) / 255.0f,
            ((color >> 24u) & 255u) / 255.0f};
        ID3D11BlendState *blend_state = lookupBlendState(presenter, draw.blend);
        if (blend_state == nullptr) {
            return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }

        presenter->context->OMSetBlendState(
            blend_state,
            blend_factor,
            0xffffffffu);
    }
    presenter->context->DrawIndexed(draw_index_count, 0u, 0);
    static const char *program_dump = std::getenv("RECOMP_D3D_PROGRAM_TEXTURE_DUMP");
    static unsigned program_dump_count = 0;
    if (draw.program_count && program_dump && program_dump_count < 2 &&
        std::getenv("RECOMP_D3D_DRAW_CAPTURE_TRIGGER") &&
        GetFileAttributesA(std::getenv("RECOMP_D3D_DRAW_CAPTURE_TRIGGER")) != INVALID_FILE_ATTRIBUTES) {
        ++program_dump_count;
        std::fprintf(stderr, "recomp program viewport: mask=%u c58=%g,%g,%g c59=%g,%g,%g\n",
            draw.program_alpha_mask ? 1u:0u, draw.program_constants[58][0],
            draw.program_constants[58][1], draw.program_constants[58][2],
            draw.program_constants[59][0],draw.program_constants[59][1],draw.program_constants[59][2]);
        RecompD3dTextureDesc output{};
        output.format_byte=0x12u; output.linear=true;
        output.width=presenter->config.width; output.height=presenter->config.height;
        dumpProgramTexture(presenter,lookupBackBufferTexture(presenter,output),
            draw.program_alpha_mask ? 3u:2u);
    }

    ++presenter->draw_count;

    /* Back-buffer dumps proved the rendered content alternates vertically by
       about 32 rows every other frame, and nothing in this file derives a
       coordinate from a frame counter. So the offset must arrive inside the
       guest transform. Log the translation row per draw for a bounded window
       so the alternation can be attributed to guest state rather than the
       host. Opt-in and off by default. */
    {
        static const char *ytrace = std::getenv("RECOMP_D3D_YTRACE");
        static unsigned ytrace_lines;

        /* One line per draw drowns in a single frame: a frame issues hundreds
           of draws, so a flat budget never reaches a second frame. The
           question is how one draw differs ACROSS frames, so sample the same
           ordinal draw of each present instead. */
        static unsigned ytrace_last_present = 0xffffffffu;
        static unsigned ytrace_ordinal;
        static const char *ord_text = std::getenv("RECOMP_D3D_YTRACE_DRAW");
        const unsigned want_ordinal = ord_text != nullptr
            ? static_cast<unsigned>(std::strtoul(ord_text, nullptr, 10))
            : 1u;

        if (presenter->present_count != ytrace_last_present) {
            ytrace_last_present = presenter->present_count;
            ytrace_ordinal = 0u;
        }
        ++ytrace_ordinal;

        if (ytrace != nullptr && ytrace_lines < 400u &&
            ytrace_ordinal == want_ordinal) {
            const float *m = draw.transform;
            float p[3] = {0.0f, 0.0f, 0.0f};

            if (draw.vertex_stride >= sizeof p) {
                std::memcpy(p, draw.vertex_bytes, sizeof p);
            }
            ++ytrace_lines;
            std::fprintf(
                stderr,
                "recomp d3d ytrace: present=%u draw=%u fvf=0x%08X "
                "m13=%g m5=%g m1=%g m9=%g v0=(%g %g %g) idx=%u\n",
                static_cast<unsigned>(presenter->present_count),
                static_cast<unsigned>(presenter->draw_count),
                static_cast<unsigned>(draw.fvf),
                m[13], m[5], m[1], m[9], p[0], p[1], p[2],
                static_cast<unsigned>(draw_index_count));
        }
    }

    if (!presenter->first_draw_reported) {
        presenter->first_draw_reported = true;
        std::fprintf(
            stderr,
            "recomp d3d presenter: first draw prim=%u indices=%u "
            "triangles=%u vertices=%u stride=%u fvf=0x%08X\n",
            static_cast<unsigned>(draw.primitive_type),
            static_cast<unsigned>(draw_index_count),
            static_cast<unsigned>(draw.triangle_count),
            static_cast<unsigned>(draw.vertex_count),
            static_cast<unsigned>(draw.vertex_stride),
            static_cast<unsigned>(draw.fvf));
        /* The draw submits but the frame stays black, so report where these
           vertices actually land in clip space. Anything outside |x|,|y| <= w
           or w <= 0 is clipped away and explains a black frame without any
           API error. */
        const float *m = draw.transform;
        std::fprintf(
            stderr,
            "recomp d3d presenter: wvp [%g %g %g %g][%g %g %g %g]"
            "[%g %g %g %g][%g %g %g %g]\n",
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
            m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        const unsigned probe = draw.vertex_count < 3u ? draw.vertex_count : 3u;
        for (unsigned v = 0u; v < probe; ++v) {
            float p[3];
            std::memcpy(
                p,
                static_cast<const unsigned char *>(draw.vertex_bytes) +
                    static_cast<size_t>(v) * draw.vertex_stride,
                sizeof p);
            const float cx = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
            const float cy = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
            const float cz = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
            const float cw = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
            std::fprintf(
                stderr,
                "recomp d3d presenter: v%u obj=(%g %g %g) clip=(%g %g %g %g)%s\n",
                v, p[0], p[1], p[2], cx, cy, cz, cw,
                (cw > 0.0f && cz >= 0.0f && cz <= cw &&
                 cx >= -cw && cx <= cw && cy >= -cw && cy <= cw)
                    ? " inside" : " OUTSIDE");
        }
    }
    // Present reports device removal; checking per draw costs a kernel call each.
    return RECOMP_D3D_PRESENTER_OK;
}

bool pumpMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0u, 0u, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

bool frameDumpDue(ULONGLONG now, unsigned interval_ms, ULONGLONG &next)
{
    if (interval_ms != 0u && now < next) {
        return false;
    }
    /* Missed captures are skipped, never replayed in a catch-up burst. */
    next = now + interval_ms;
    return true;
}

void writeFrameDump(RecompD3dPresenter *presenter, ID3D11Texture2D *staging,
    const char *path, unsigned present_count, ULONGLONG captured_ms)
{
    D3D11_TEXTURE2D_DESC desc{};
    staging->GetDesc(&desc);
    const unsigned width = desc.Width;
    const unsigned height = desc.Height;
    const unsigned row_bytes = width * 3u;
    const unsigned padded = (row_bytes + 3u) & ~3u;
    const unsigned image_bytes = padded * height;
    /* Allocate before mapping or opening the file, so failure leaves no partial BMP. */
    // A Debug vector also allocates an iterator proxy in its noexcept constructor.
    std::unique_ptr<unsigned char[]> bmp_row(new (std::nothrow) unsigned char[padded]{});
    if (!bmp_row) {
        std::fprintf(stderr, "recomp frame dump: row allocation failed path=%s\n", path);
        return;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(presenter->context->Map(
            staging, 0u, D3D11_MAP_READ, 0u, &mapped))) {

        if (FILE *file = std::fopen(path, "wb")) {
            unsigned char header[54] = {0};
            const unsigned total = 54u + image_bytes;

            header[0] = 'B'; header[1] = 'M';
            std::memcpy(header + 2, &total, 4);
            const unsigned offset = 54u;
            std::memcpy(header + 10, &offset, 4);
            const unsigned info_size = 40u;
            std::memcpy(header + 14, &info_size, 4);
            std::memcpy(header + 18, &width, 4);
            std::memcpy(header + 22, &height, 4);
            const unsigned short planes = 1u;
            std::memcpy(header + 26, &planes, 2);
            const unsigned short bpp = 24u;
            std::memcpy(header + 28, &bpp, 2);
            std::memcpy(header + 34, &image_bytes, 4);
            std::fwrite(header, 1, sizeof header, file);

            /* Write a whole row: per-pixel stdio locking stalls capture replay. */
            for (unsigned y = 0u; y < height; ++y) {
                const unsigned char *row =
                    static_cast<const unsigned char *>(mapped.pData) +
                    static_cast<size_t>(height - 1u - y) * mapped.RowPitch;
                for (unsigned x = 0u; x < width; ++x) {
                    /* Back buffer is B8G8R8A8, which already matches BMP order. */
                    std::memcpy(bmp_row.get()+x*3u, row+x*4u, 3u);
                }
                std::fwrite(bmp_row.get(), 1, padded, file);
            }
            std::fclose(file);
            std::fprintf(
                stderr,
                "recomp d3d presenter: frame dump path=%s size=%ux%u present=%u captured_ms=%llu\n",
                path, width, height,
                static_cast<unsigned>(present_count), static_cast<unsigned long long>(captured_ms));
        }
        presenter->context->Unmap(staging, 0u);
    }
}

void flushFrameDumps(RecompD3dPresenter *presenter)
{
    for (const auto &dump : presenter->deferred_dumps)
        writeFrameDump(presenter, dump.texture.get(), dump.path.c_str(), dump.present, dump.captured_ms);
    presenter->deferred_dumps.clear();
}

bool frameDumpFits(const D3D11_TEXTURE2D_DESC &desc, unsigned count)
{
    return count <= 300 && uint64_t(desc.Width)*desc.Height*4*count <= 12ull*1024*1024*1024;
}

/* RECOMP_D3D_FRAME_DUMP names the BMP path; AT and COUNT select presents.
   INTERVAL_MS optionally spaces captures in host time. TRIGGER names a file
   whose appearance starts each burst of the length it contains; the file is
   deleted once the burst is written, so scripted navigation can capture
   several scenes in one run.
   Capture stays inside the renderer, without cross-process window painting
   or missed-frame bursts. */
void dumpBackBufferOnce(RecompD3dPresenter *presenter, uint32_t present_count)
{
    const char *path = std::getenv("RECOMP_D3D_FRAME_DUMP");
    if (path == nullptr) {
        return;
    }
    /* A single dump cannot answer whether content moves between frames, only
       whether one frame is correct. RECOMP_D3D_FRAME_DUMP_COUNT captures a
       burst of consecutive presents so successive back buffers can be
       compared against each other. Default 1 keeps existing gates identical. */
    const char *count_text = std::getenv("RECOMP_D3D_FRAME_DUMP_COUNT");
    unsigned count = count_text != nullptr
        ? static_cast<unsigned>(std::strtoul(count_text, nullptr, 10))
        : 1u;
    const char *trigger = std::getenv("RECOMP_D3D_FRAME_DUMP_TRIGGER");
    if (trigger != nullptr) {
        count = presenter->frame_dump_burst_count;
    } else if (count > 300) {
        const char *defer = std::getenv("RECOMP_D3D_FRAME_DUMP_DEFER");
        if (defer && std::strcmp(defer, "1") == 0) return;
    }
    if (presenter->frame_dump_count - presenter->frame_dump_burst_base >=
            (count == 0u ? 1u : count)) {
        // Flush on the following frame so every captured frame was presented first.
        flushFrameDumps(presenter);
        if (trigger != nullptr) {
            DeleteFileA(trigger);
            presenter->frame_dump_burst_base = presenter->frame_dump_count;
            presenter->frame_dump_burst_count = 0u;
        }
        return;
    }
    if (trigger != nullptr && count == 0u) {
        FILE *file = std::fopen(trigger, "rb");
        if (file == nullptr) {
            return;
        }
        unsigned requested = 0u;
        if (std::fscanf(file, "%u", &requested) != 1 || requested == 0u) {
            requested = 1u;
        }
        std::fclose(file);
        count = presenter->frame_dump_burst_count = requested;
        const char *defer = std::getenv("RECOMP_D3D_FRAME_DUMP_DEFER");
        ID3D11Texture2D *back_buffer = nullptr;
        if (defer && std::strcmp(defer, "1") == 0 && SUCCEEDED(
                presenter->swap_chain->GetBuffer(0u, __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void **>(&back_buffer)))) {
            D3D11_TEXTURE2D_DESC desc{};
            back_buffer->GetDesc(&desc);
            back_buffer->Release();
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0u;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0u;
            if (!frameDumpFits(desc, requested)) {
                std::fprintf(stderr, "recomp frame dump: deferred capture exceeds frame dump bound\n");
                DeleteFileA(trigger);
                presenter->frame_dump_burst_base = presenter->frame_dump_count;
                presenter->frame_dump_burst_count = 0u;
                return;
            }
            for (unsigned i = 0u; i < requested; ++i) {
                ID3D11Texture2D *staging = nullptr;
                if (FAILED(presenter->device->CreateTexture2D(&desc, nullptr, &staging))) break;
                presenter->dump_pool.emplace_back(staging);
            }
            return; // Start the burst on the next present, after the allocation hitch.
        }
    }
    const char *at_text = std::getenv("RECOMP_D3D_FRAME_DUMP_AT");
    // A comma list ("1800,3600,5400") takes dump k at the k-th listed present.
    char *at_next = nullptr;
    unsigned at = at_text != nullptr
        ? static_cast<unsigned>(std::strtoul(at_text, &at_next, 10))
        : 1u;
    for (unsigned i = 0u; at_next != nullptr && *at_next == ',' &&
            i < presenter->frame_dump_count - presenter->frame_dump_burst_base; ++i) {
        at = static_cast<unsigned>(std::strtoul(at_next + 1, &at_next, 10));
    }
    if (present_count < at) {
        return;
    }
    const char *interval_text =
        std::getenv("RECOMP_D3D_FRAME_DUMP_INTERVAL_MS");
    const unsigned interval_ms = interval_text != nullptr
        ? static_cast<unsigned>(std::strtoul(interval_text, nullptr, 10))
        : 0u;
    if (!frameDumpDue(GetTickCount64(), interval_ms,
            presenter->next_frame_dump_ms)) {
        return;
    }
    const unsigned dump_index = presenter->frame_dump_count++;

    /* One name per frame in a burst; the single-dump case keeps the exact
       path it always used so existing gates and receipts still match. */
    char burst_path[1024];
    if (count > 1u || trigger != nullptr) {
        std::snprintf(
            burst_path, sizeof burst_path, "%s.%03u.bmp", path, dump_index);
        path = burst_path;
    }

    ID3D11Texture2D *back_buffer = nullptr;
    if (FAILED(presenter->swap_chain->GetBuffer(
            0u, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void **>(&back_buffer)))) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    back_buffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0u;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0u;

    ID3D11Texture2D *staging = nullptr;
    if (!presenter->dump_pool.empty()) {
        staging = presenter->dump_pool.back().release();
        presenter->dump_pool.pop_back();
    } else if (FAILED(presenter->device->CreateTexture2D(&desc, nullptr, &staging))) {
        back_buffer->Release();
        return;
    }
    presenter->context->CopyResource(staging, back_buffer);

    back_buffer->Release();
    std::unique_ptr<ID3D11Texture2D, DumpTextureRelease> owned(staging);
    const char *defer = std::getenv("RECOMP_D3D_FRAME_DUMP_DEFER");
    if (defer && std::strcmp(defer, "1") == 0) {
        // Bound this diagnostic to 300 4K frames (about 10 GiB of readback memory).
        if (!frameDumpFits(desc, count)) {
            std::fprintf(stderr, "recomp frame dump: deferred capture exceeds frame dump bound\n");
            return;
        }
        presenter->deferred_dumps.push_back({std::move(owned), path, present_count, GetTickCount64()});
    } else writeFrameDump(presenter, owned.get(), path, present_count, GetTickCount64());
    presenter->next_frame_dump_ms = GetTickCount64() + interval_ms;
}

RecompD3dPresenterError submitGamma(
    RecompD3dPresenter *presenter, const uint8_t (&ramp)[3][256])
{
    float values[256][4]{};
    bool enabled = false;
    for (unsigned i = 0u; i < 256u; ++i) {
        for (unsigned channel = 0u; channel < 3u; ++channel) {
            values[i][channel] = ramp[channel][i] / 255.0f;
            enabled |= ramp[channel][i] != i;
        }
    }
    if (enabled && presenter->gamma_buffer == nullptr) {
        static const char shader[] =
            "Texture2D pixels : register(t0);\n"
            "cbuffer Gamma : register(b0) { float4 ramp[256]; };\n"
            "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
            " float2 p = float2((id << 1) & 2, id & 2);\n"
            " return float4(p * float2(2,-2) + float2(-1,1), 0, 1); }\n"
            "float4 ps(float4 p : SV_Position) : SV_Target {\n"
            " float4 c = pixels.Load(int3(p.xy,0));\n"
            " uint3 i = (uint3)(saturate(c.rgb) * 255 + 0.5);\n"
            " return float4(ramp[i.r].r,ramp[i.g].g,ramp[i.b].b,c.a); }\n";
        ID3DBlob *vertex = nullptr, *pixel = nullptr;
        HRESULT result = D3DCompile(shader, sizeof shader - 1u, nullptr,
            nullptr, nullptr, "vs", "vs_4_0", 0u, 0u, &vertex, nullptr);
        if (SUCCEEDED(result)) result = D3DCompile(shader, sizeof shader - 1u,
            nullptr, nullptr, nullptr, "ps", "ps_4_0", 0u, 0u, &pixel, nullptr);
        if (SUCCEEDED(result)) result = presenter->device->CreateVertexShader(
            vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr,
            &presenter->gamma_vertex_shader);
        if (SUCCEEDED(result)) result = presenter->device->CreatePixelShader(
            pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr,
            &presenter->gamma_pixel_shader);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof values;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(result)) result = presenter->device->CreateBuffer(
            &desc, nullptr, &presenter->gamma_buffer);
        releaseCom(vertex);
        releaseCom(pixel);
        if (FAILED(result)) {
            releaseCom(presenter->gamma_vertex_shader);
            releaseCom(presenter->gamma_pixel_shader);
            releaseCom(presenter->gamma_buffer);
            return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }
    }
    if (enabled) presenter->context->UpdateSubresource(
        presenter->gamma_buffer, 0u, nullptr, values, 0u, 0u);
    presenter->gamma_enabled = enabled;
    return RECOMP_D3D_PRESENTER_OK;
}

/* SMAA 1x (iryoku/smaa, ultra preset) on the final resolved image. */
bool createSmaa(RecompD3dPresenter *presenter)
{
#ifdef RECOMP_SMAA
    static const char prefix[] =
        "#define SMAA_CUSTOM_SL\n#define SMAA_PRESET_ULTRA\n"
        "SamplerState LinearSampler : register(s0);\n"
        "SamplerState PointSampler : register(s1);\n"
        "#define SMAATexture2D(tex) Texture2D tex\n"
        "#define SMAATexturePass2D(tex) tex\n"
        "#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)\n"
        "#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)\n"
        "#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)\n"
        "#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)\n"
        "#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)\n"
        "#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)\n"
        "#define SMAA_FLATTEN [flatten]\n#define SMAA_BRANCH [branch]\n";
    static const char suffix[] =
        "\nTexture2D colorTex : register(t0); Texture2D edgesTex : register(t1);\n"
        "Texture2D areaTex : register(t2); Texture2D searchTex : register(t3);\n"
        "Texture2D blendTex : register(t4);\n"
        "void fullscreen(uint id, out float4 pos, out float2 uv) {\n"
        " uv = float2((id << 1) & 2, id & 2); pos = float4(uv * float2(2,-2) + float2(-1,1), 0, 1); }\n"
        "void edgeVS(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0,\n"
        " out float4 offset[3] : TEXCOORD1) { fullscreen(id, pos, uv); SMAAEdgeDetectionVS(uv, offset); }\n"
        "float2 edgePS(float4 pos : SV_Position, float2 uv : TEXCOORD0, float4 offset[3] : TEXCOORD1)\n"
        " : SV_Target { return SMAALumaEdgeDetectionPS(uv, offset, colorTex); }\n"
        "void weightVS(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0,\n"
        " out float2 pix : TEXCOORD1, out float4 offset[3] : TEXCOORD2) {\n"
        " fullscreen(id, pos, uv); SMAABlendingWeightCalculationVS(uv, pix, offset); }\n"
        "float4 weightPS(float4 pos : SV_Position, float2 uv : TEXCOORD0, float2 pix : TEXCOORD1,\n"
        " float4 offset[3] : TEXCOORD2) : SV_Target {\n"
        " return SMAABlendingWeightCalculationPS(uv, pix, offset, edgesTex, areaTex, searchTex, 0); }\n"
        "void blendVS(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0,\n"
        " out float4 offset : TEXCOORD1) { fullscreen(id, pos, uv); SMAANeighborhoodBlendingVS(uv, offset); }\n"
        "float4 blendPS(float4 pos : SV_Position, float2 uv : TEXCOORD0, float4 offset : TEXCOORD1)\n"
        " : SV_Target { return SMAANeighborhoodBlendingPS(uv, offset, colorTex, blendTex); }\n";
    const uint32_t width = mainWidth(presenter), height = mainHeight(presenter);
    char metrics[128];
    std::snprintf(metrics, sizeof metrics, "float4(1.0/%u.0,1.0/%u.0,%u.0,%u.0)",
        width, height, width, height);
    const D3D_SHADER_MACRO macros[] = {{"SMAA_RT_METRICS", metrics}, {nullptr, nullptr}};
    std::string source = prefix;
    source.append(kSmaaSource, sizeof kSmaaSource);
    source += suffix;
    static const char *const entries[3][2] = {
        {"edgeVS", "edgePS"}, {"weightVS", "weightPS"}, {"blendVS", "blendPS"}};
    HRESULT result = S_OK;
    for (unsigned pass = 0u; pass < 3u && SUCCEEDED(result); ++pass) {
        ID3DBlob *vertex = nullptr, *pixel = nullptr, *errors = nullptr;
        result = D3DCompile(source.data(), source.size(), "SMAA.hlsl", macros, nullptr,
            entries[pass][0], "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u, &vertex, &errors);
        if (SUCCEEDED(result)) {
            releaseCom(errors);
            result = D3DCompile(source.data(), source.size(), "SMAA.hlsl", macros, nullptr,
                entries[pass][1], "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u, &pixel, &errors);
        }
        if (FAILED(result) && errors != nullptr) {
            std::fprintf(stderr, "recomp d3d presenter: smaa compile: %s\n",
                static_cast<const char *>(errors->GetBufferPointer()));
        }
        if (SUCCEEDED(result)) result = presenter->device->CreateVertexShader(
            vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr,
            &presenter->smaa_vs[pass]);
        if (SUCCEEDED(result)) result = presenter->device->CreatePixelShader(
            pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr,
            &presenter->smaa_ps[pass]);
        releaseCom(vertex);
        releaseCom(pixel);
        releaseCom(errors);
    }
    const struct { UINT width, height; DXGI_FORMAT format; const void *data; UINT pitch; }
        textures[5] = {
            {width, height, DXGI_FORMAT_R8G8_UNORM, nullptr, 0u},
            {AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, areaTexBytes, AREATEX_PITCH},
            {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, searchTexBytes,
             SEARCHTEX_PITCH},
            {width, height, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 0u},
            {width, height, DXGI_FORMAT_B8G8R8A8_UNORM, nullptr, 0u},
        };
    unsigned target = 0u;
    for (unsigned i = 0u; i < 5u && SUCCEEDED(result); ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = textures[i].width;
        desc.Height = textures[i].height;
        desc.MipLevels = desc.ArraySize = 1u;
        desc.Format = textures[i].format;
        desc.SampleDesc.Count = 1u;
        desc.Usage = textures[i].data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
            (textures[i].data ? 0u : D3D11_BIND_RENDER_TARGET);
        const D3D11_SUBRESOURCE_DATA data = {textures[i].data, textures[i].pitch, 0u};
        ID3D11Texture2D *texture = nullptr;
        result = presenter->device->CreateTexture2D(
            &desc, textures[i].data ? &data : nullptr, &texture);
        if (SUCCEEDED(result)) result = presenter->device->CreateShaderResourceView(
            texture, nullptr, &presenter->smaa_views[i + 1u]);
        if (SUCCEEDED(result) && textures[i].data == nullptr) {
            result = presenter->device->CreateRenderTargetView(
                texture, nullptr, &presenter->smaa_targets[target++]);
        }
        releaseCom(texture);
    }
    for (unsigned i = 0u; i < 2u && SUCCEEDED(result); ++i) {
        D3D11_SAMPLER_DESC desc{};
        desc.Filter = i == 0u
            ? D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        result = presenter->device->CreateSamplerState(&desc, &presenter->smaa_samplers[i]);
    }
    if (FAILED(result)) {
        std::fprintf(stderr, "recomp d3d presenter: smaa create failed hr=0x%08lX\n",
            static_cast<unsigned long>(result));
        releaseSmaa(presenter);
        return false;
    }
    std::fprintf(stderr, "recomp d3d presenter: smaa ultra %ux%u\n", width, height);
    return true;
#else
    (void)presenter;
    return false;
#endif
}

void runSmaa(RecompD3dPresenter *presenter, ID3D11ShaderResourceView *color,
    ID3D11RenderTargetView *output)
{
    auto *context = presenter->context;
    const float zero[4]{};
    context->ClearRenderTargetView(presenter->smaa_targets[0], zero);
    context->ClearRenderTargetView(presenter->smaa_targets[1], zero);
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f,
        static_cast<float>(mainWidth(presenter)),
        static_cast<float>(mainHeight(presenter)), 0.0f, 1.0f};
    context->RSSetViewports(1u, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->PSSetSamplers(0u, 2u, presenter->smaa_samplers);
    ID3D11RenderTargetView *targets[3] = {
        presenter->smaa_targets[0], presenter->smaa_targets[1], output};
    for (unsigned pass = 0u; pass < 3u; ++pass) {
        ID3D11ShaderResourceView *views[5] = {color, presenter->smaa_views[1],
            presenter->smaa_views[2], presenter->smaa_views[3], presenter->smaa_views[4]};
        if (pass == 0u) views[1] = nullptr;
        if (pass < 2u) views[4] = nullptr; // never sample the target being drawn
        ID3D11ShaderResourceView *none[5]{};
        context->PSSetShaderResources(0u, 5u, none);
        context->OMSetRenderTargets(1u, &targets[pass], nullptr);
        context->PSSetShaderResources(0u, 5u, views);
        context->VSSetShader(presenter->smaa_vs[pass], nullptr, 0u);
        context->PSSetShader(presenter->smaa_ps[pass], nullptr, 0u);
        context->Draw(3u, 0u);
    }
}

bool renderOutput(RecompD3dPresenter *presenter, ID3D11RenderTargetView *output)
{
    if (output == nullptr) return false;
    auto *context = presenter->context;
    context->ClearState();
    if (presenter->smaa && presenter->smaa_vs[0] == nullptr && !createSmaa(presenter)) {
        std::fprintf(stderr, "recomp d3d presenter: smaa unavailable\n");
        presenter->smaa = false;
    }
    if (!presenter->gamma_enabled && !presenter->smaa) {
        ID3D11Resource *target = nullptr;
        output->GetResource(&target);
        copyGuestBuffer(presenter, target);
        releaseCom(target);
        return true;
    }
    RecompD3dTextureDesc desc{};
    desc.format_byte = 0x12u;
    desc.linear = true;
    desc.width = presenter->config.width;
    desc.height = presenter->config.height;
    ID3D11ShaderResourceView *source = lookupBackBufferTexture(presenter, desc);
    if (source == nullptr) return false;
    if (presenter->smaa) {
        runSmaa(presenter, source,
            presenter->gamma_enabled ? presenter->smaa_targets[2] : output);
        if (!presenter->gamma_enabled) {
            context->ClearState();
            return true;
        }
        source = presenter->smaa_views[5];
    }
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f,
        static_cast<float>(mainWidth(presenter)),
        static_cast<float>(mainHeight(presenter)), 0.0f, 1.0f};
    context->RSSetViewports(1u, &viewport);
    context->OMSetRenderTargets(1u, &output, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(presenter->gamma_vertex_shader, nullptr, 0u);
    context->PSSetShader(presenter->gamma_pixel_shader, nullptr, 0u);
    context->PSSetConstantBuffers(0u, 1u, &presenter->gamma_buffer);
    context->PSSetShaderResources(0u, 1u, &source);
    context->Draw(3u, 0u);
    context->ClearState();
    return true;
}

bool compareReplay(RecompD3dPresenter *presenter)
{
    if (!presenter->replay_verify_frame) return true;
    ID3D11Texture2D *back = nullptr, *staging = nullptr;
    if (FAILED(presenter->swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void **>(&back)))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    HRESULT result = presenter->device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(result)) { back->Release(); return false; }
    presenter->context->CopyResource(staging, back);
    back->Release();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = presenter->context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) { staging->Release(); return false; }
    const size_t row_bytes = size_t(desc.Width)*4;
    auto &reference = presenter->replay_reference;
    try {
        if (!presenter->replay_verify_second) reference.resize(row_bytes*desc.Height);
    } catch (...) {
        presenter->context->Unmap(staging, 0);
        staging->Release();
        throw;
    }
    bool valid = reference.size() == row_bytes*desc.Height;
    uint64_t changed = 0;
    unsigned maximum = 0;
    if (valid) for (unsigned y = 0; y < desc.Height; ++y) {
        const auto *row = static_cast<const uint8_t *>(mapped.pData)+size_t(y)*mapped.RowPitch;
        auto *old = reference.data()+size_t(y)*row_bytes;
        if (!presenter->replay_verify_second) std::memcpy(old, row, row_bytes);
        else for (unsigned x = 0; x < desc.Width; ++x) {
            bool different = false;
            for (unsigned c = 0; c < 3; ++c) {
                unsigned delta = static_cast<unsigned>(std::abs(int(row[x*4+c])-int(old[x*4+c])));
                maximum = (std::max)(maximum, delta);
                different |= delta != 0;
            }
            changed += different;
        }
    }
    presenter->context->Unmap(staging, 0);
    staging->Release();
    if (presenter->replay_verify_second) std::fprintf(stderr,
        "recomp replay identity: frame=%u pixels=%llu changed=%llu max_channel=%u valid=%u\n",
        presenter->replay_verify_frame, static_cast<unsigned long long>(desc.Width)*desc.Height,
        static_cast<unsigned long long>(changed), maximum, valid);
    presenter->replay_verify_frame = 0;
    return valid && changed == 0;
}

/* The guest runs at 60 Hz. On a 120 or 240 Hz display, interval 1 shows its
   frames for an uneven 1-3 or 3-5 refreshes (judder); hold each for exactly
   refresh/60. Rechecked each second, as the window can change monitors.
   ponytail: other rates keep interval 1; only VRR can pace 60 Hz evenly there. */
UINT fixedRefreshInterval(UINT hz)
{
    // Rates below the multiple cannot retire sixty frames per second.
    const UINT k = (hz + 30u) / 60u;
    return (k == 2u || k == 4u) && hz >= 60u * k && hz <= 60u * k + 1u ? k : 1u;
}

UINT syncInterval(RecompD3dPresenter *presenter)
{
    if (split_presentation) return 1u;
    const auto now = std::chrono::steady_clock::now();
    if (now >= presenter->next_refresh_check) {
        presenter->next_refresh_check = now + std::chrono::seconds(1);
        MONITORINFOEXW monitor{};
        monitor.cbSize = sizeof monitor;
        DEVMODEW mode{};
        mode.dmSize = sizeof mode;
        UINT interval = 1u;
        if (GetMonitorInfoW(MonitorFromWindow(presenter->window, MONITOR_DEFAULTTONEAREST), &monitor) &&
            EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
            interval = fixedRefreshInterval(mode.dmDisplayFrequency);
        }
        presenter->sync_interval = interval;
    }
    return presenter->sync_interval;
}

void recordFrameStatistics(RecompD3dPresenter *presenter,
    HRESULT result, const DXGI_FRAME_STATISTICS &stats)
{
    if (FAILED(result)) {
        presenter->last_stat_present = presenter->last_stat_refresh = 0u;
        return;
    }
    if (presenter->last_stat_present != 0u &&
        stats.PresentCount == presenter->last_stat_present + 1u) {
        const UINT hold = stats.PresentRefreshCount - presenter->last_stat_refresh;
        ++presenter->refresh_holds[(std::min)(hold, 8u)];
    }
    presenter->last_stat_present = stats.PresentCount;
    presenter->last_stat_refresh = stats.PresentRefreshCount;
}

RecompD3dPresenterError submitPresent(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterPresentCommand &present)
{
    if (present.effective_flags != 5u || present.swap_counter == 0u) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    /* Three distinct conditions used to collapse into one HOST_FAILURE code,
       so a stop here named the symptom and not the cause. Name each one. */
    {
        const bool pumped = pumpMessages();
        if (pumped && presenter->close_requested) {
            return RECOMP_D3D_PRESENTER_CLOSED;
        }
        if (!pumped || !IsWindow(presenter->window)) {
            std::fprintf(
                stderr,
                "recomp d3d presenter: present precondition failed "
                "wm_quit=%d is_window=%d present_count=%u swap=%u "
                "last_error=%lu\n",
                pumped ? 0 : 1,
                IsWindow(presenter->window) ? 1 : 0,
                static_cast<unsigned>(presenter->present_count),
                static_cast<unsigned>(present.swap_counter),
                GetLastError());
            return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }
    }

    /* SyncInterval, not the flag, is what paces a present. Passing 0 here
       unconditionally meant the throttled path was never actually throttled:
       every frame was retired immediately and only the blocking behaviour
       changed. Pace to one refresh unless immediate presenting is asked for. */
    if (!renderOutput(presenter, presenter->vrr_target_view != nullptr
            ? presenter->vrr_target_view : presenter->present_target_view)) {
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    copyFrontBuffer(presenter);
    if (presenter->vrr_target_view != nullptr) {
        auto *context = presenter->context;
        D3D11_TEXTURE2D_DESC window{};
        ID3D11Texture2D *back_buffer = nullptr;
        presenter->swap_chain->GetBuffer(0u, IID_PPV_ARGS(&back_buffer));
        if (back_buffer == nullptr) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        back_buffer->GetDesc(&window);
        releaseCom(back_buffer);
        const D3D11_VIEWPORT viewport = {0.0f, 0.0f,
            static_cast<float>(window.Width), static_cast<float>(window.Height), 0.0f, 1.0f};
        context->ClearState();
        context->RSSetViewports(1u, &viewport);
        context->OMSetRenderTargets(1u, &presenter->present_target_view, nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(presenter->vrr_vs, nullptr, 0u);
        context->PSSetShader(presenter->vrr_ps, nullptr, 0u);
        context->PSSetSamplers(0u, 1u, &presenter->vrr_sampler);
        context->PSSetShaderResources(0u, 1u, &presenter->vrr_source);
        context->Draw(3u, 0u);
        context->ClearState();
    }
    // Capture the rendered buffer before flip presentation releases it.
    if (!compareReplay(presenter)) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    dumpBackBufferOnce(presenter, presenter->present_count + 1u);

    const auto clock_ms = [] {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const double present_start_ms = presenter->performance_counter ? clock_ms() : 0.0;
    const bool immediate = immediatePresent(presenter);
    const UINT sync_interval = immediate || presenter->vrr ? 0u : syncInterval(presenter);
    const UINT present_flags = immediate ? DXGI_PRESENT_DO_NOT_WAIT
        : presenter->vrr ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    if (presenter->vrr && !split_presentation) {
        /* VRR shows a frame the moment it is presented, so render-time jitter
           became 12-23 ms frames. Present on the guest's exact 1/60 s grid; a
           late frame presents now and moves the grid, as the guest timer does. */
        const long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        presenter->vrr_slot_ns = (std::max)(presenter->vrr_slot_ns + 1000000000 / 60, now);
        // Submit the frame's last commands now, so the GPU finishes them before the flip.
        presenter->context->Flush();
        recomp_d3d_sleep_until(presenter->vrr_slot_ns);
    }
    LARGE_INTEGER qpc_before{}, qpc_after{};
    QueryPerformanceCounter(&qpc_before);
    const HRESULT present_result = presenter->swap_chain->Present(sync_interval, present_flags);
    QueryPerformanceCounter(&qpc_after);
    if (FAILED(present_result)) {
        std::fprintf(
            stderr,
            "recomp d3d presenter: Present failed hr=0x%08lX "
            "removed_reason=0x%08lX present_count=%u swap=%u\n",
            static_cast<unsigned long>(present_result),
            static_cast<unsigned long>(
                presenter->device->GetDeviceRemovedReason()),
            static_cast<unsigned>(presenter->present_count),
            static_cast<unsigned>(present.swap_counter));
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    ++presenter->present_count;
    DXGI_FRAME_STATISTICS stats{};
    const HRESULT stats_result = presenter->present_log != nullptr || presenter->performance_counter
        ? presenter->swap_chain->GetFrameStatistics(&stats) : E_FAIL;
    if (presenter->present_log != nullptr) {
        UINT last_present = 0u;
        presenter->swap_chain->GetLastPresentCount(&last_present);
        std::fprintf(presenter->present_log,
            "%u,%lld,%lld,%u,0x%X,0x%08lX,%u,0x%08lX,%u,%u,%u,%lld\n",
            presenter->present_count, qpc_before.QuadPart, qpc_after.QuadPart,
            sync_interval, present_flags, static_cast<unsigned long>(present_result),
            last_present, static_cast<unsigned long>(stats_result), stats.PresentCount,
            stats.PresentRefreshCount, stats.SyncRefreshCount, stats.SyncQPCTime.QuadPart);
        std::fflush(presenter->present_log);
    }
    if (presenter->performance_counter) {
        const double present_end_ms = clock_ms();
        // The first Present lands before the sampled window opens.
        if (presenter->frame_rate.started) {
            presenter->present_call_max_ms = (std::max)(
                presenter->present_call_max_ms, present_end_ms - present_start_ms);
        }
        if (presenter->last_present_ms != 0.0) {
            presenter->present_gaps.push_back(present_end_ms - presenter->last_present_ms);
        }
        presenter->last_present_ms = present_end_ms;
        recordFrameStatistics(presenter, stats_result, stats);
        double fps, frame_ms;
        const ULONGLONG now = GetTickCount64();
        if (sampleFrameRate(presenter->frame_rate, now, fps, frame_ms)) {
            char title[96];
            std::snprintf(title, sizeof title, "DOAXBV Recomp | %.1f FPS | %.1f ms/frame", fps, frame_ms);
            SetWindowTextA(presenter->window, title);
            // A late frame took over 1.5x this second's average: visible judder.
            double max_ms = 0.0;
            unsigned late = 0u;
            for (const double gap : presenter->present_gaps) {
                max_ms = (std::max)(max_ms, gap);
                late += gap > frame_ms * 1.5;
            }
            std::fprintf(stderr, "recomp performance: tick_ms=%llu present=%u fps=%.2f frame_ms=%.3f "
                "max_ms=%.1f late=%u present_max_ms=%.1f holds=%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                static_cast<unsigned long long>(now), presenter->present_count, fps, frame_ms,
                max_ms, late, presenter->present_call_max_ms,
                presenter->refresh_holds[0], presenter->refresh_holds[1], presenter->refresh_holds[2],
                presenter->refresh_holds[3], presenter->refresh_holds[4], presenter->refresh_holds[5],
                presenter->refresh_holds[6], presenter->refresh_holds[7], presenter->refresh_holds[8]);
            std::fill(std::begin(presenter->refresh_holds), std::end(presenter->refresh_holds), 0u);
            presenter->present_gaps.clear();
            presenter->present_call_max_ms = 0.0;
        }
    }


    /* Screen-scraping the presenter window proved unreliable as a gate: the
       captured rectangle is whatever is topmost at that screen position, so a
       run can "pass" on desktop pixels. Reading the back buffer here measures
       what this program actually rendered. Opt-in, and off by default. */

    if (!presenter->first_present_reported) {
        ShowWindow(presenter->window, SW_SHOW);
        UpdateWindow(presenter->window);
        if (!pumpMessages()) {
            return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }
        if (presenter->close_requested) {
            return RECOMP_D3D_PRESENTER_CLOSED;
        }
        if (!IsWindow(presenter->window)) {
            return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }
        RECT client_rect{};
        if (!GetClientRect(presenter->window, &client_rect) ||
            !IsWindowVisible(presenter->window)) {
            return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }
        std::fprintf(
            stderr,
            "recomp d3d presenter: creation/first-present hwnd=%p "
            "client=%ldx%ld visible=1 driver=%s create_hr=0x%08lX "
            "present_hr=0x%08lX count=%u\n",
            static_cast<void *>(presenter->window),
            client_rect.right - client_rect.left,
            client_rect.bottom - client_rect.top,
            presenter->driver_name,
            static_cast<unsigned long>(presenter->create_result),
            static_cast<unsigned long>(present_result),
            static_cast<unsigned>(presenter->present_count));
        presenter->first_present_reported = true;
    }
    return RECOMP_D3D_PRESENTER_OK;
}

} // namespace

RecompD3dPresenterError d3d11_backend_create(
    const RecompD3dPresenterConfig *config,
    RecompD3dPresenter **presenter)
{
    if (config == nullptr || presenter == nullptr || !configSupported(*config)) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    if (*presenter != nullptr || active_presenter != nullptr) {
        return RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED;
    }

    RecompD3dPresenter *created = nullptr;
    try {
        created = new RecompD3dPresenter{};
    } catch (const std::bad_alloc &) {
        return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    }
    created->config = *config;
    const char *widescreen = std::getenv("RECOMP_D3D_WIDESCREEN");
    created->widescreen = widescreen == nullptr || std::strcmp(widescreen, "0") != 0;
    const char *performance = std::getenv("RECOMP_PERF_COUNTER");
    created->performance_counter = performance != nullptr && std::strcmp(performance, "1") == 0;
    if (const char *scale = std::getenv("RECOMP_D3D_SCALE")) {
        const float value = static_cast<float>(std::atof(scale));
        if (std::isfinite(value)) created->scale = std::clamp(value, 1.0f, 8.0f);
    }
    if (const char *msaa = std::getenv("RECOMP_D3D_MSAA")) {
        created->msaa = std::clamp(std::atoi(msaa), 1, 32);
    }
    const char *smaa = std::getenv("RECOMP_D3D_SMAA");
    created->smaa = smaa != nullptr && std::strcmp(smaa, "0") != 0;
    /* VRR shows each frame when it is presented, so any VRR display paces 60 Hz
       evenly. DXGI cannot tell whether VRR is active; without it this tears. */
    const char *vrr = std::getenv("RECOMP_D3D_VRR");
    if (vrr != nullptr && std::strcmp(vrr, "1") == 0) {
        IDXGIFactory5 *factory = nullptr;
        BOOL tearing = FALSE;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
            factory->Release();
        }
        created->vrr = tearing != FALSE;
        std::fprintf(stderr, "recomp d3d presenter: vrr %s\n", created->vrr ? "on" : "unsupported");
    }
    if (const char *log = std::getenv("RECOMP_D3D_PRESENT_LOG")) {
        created->present_log = std::fopen(log, "w");
        if (created->present_log != nullptr) {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            std::fprintf(created->present_log, "# qpc_frequency=%lld\n"
                "present,qpc_before,qpc_after,sync_interval,flags,present_hr,last_present_count,"
                "stats_hr,stats_present_count,present_refresh_count,sync_refresh_count,sync_qpc\n",
                frequency.QuadPart);
        }
    }
    created->owner_thread = GetCurrentThreadId();
    if (!createWindow(created)) {
        releasePresenter(created);
        delete created;
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    const HRESULT create_result = createGraphics(created);
    if (FAILED(create_result)) {
        releasePresenter(created);
        delete created;
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    try {
        created->precompile = std::thread(precompileDrawShaders, &created->precompile_stop);
    } catch (const std::system_error &) {
        // Shaders then compile on first use, as before.
    }

    active_presenter = created;
    *presenter = created;
    return RECOMP_D3D_PRESENTER_OK;
}

/* Creates a cache-miss texture before its draw needs it. Addresses already
   cached, render targets and dynamic linear textures are skipped, so entries
   the packet on screen still uses are not replaced. */
bool textureMissing(RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand &draw)
{
    const RecompD3dTextureDesc &desc = draw.texture;
    return !draw.texture_is_backbuffer && !desc.linear && draw.texture_bytes != nullptr &&
        presenter->texture_index.count(desc.data) == 0u &&
        findRenderTarget(presenter, desc) == nullptr;
}

void prepareTexture(RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand &draw)
{
    if (textureMissing(presenter, draw)) lookupTexture(presenter, draw);
}

/* The draw's main texture plus its alpha mask or reflection texture, as drawn. */
template <typename Visit>
void visitPrepareTextures(const RecompD3dPresenterDrawCommand &draw, Visit visit)
{
    if (draw.has_texture) visit(draw);
    RecompD3dPresenterDrawCommand extra{};
    if (draw.has_alpha_mask) {
        extra.texture = draw.alpha_mask;
        extra.texture_bytes = draw.alpha_mask_bytes;
        extra.texture_byte_count = draw.alpha_mask_byte_count;
        extra.palette_bytes = draw.alpha_mask_palette;
        extra.palette_byte_count = draw.alpha_mask_palette_byte_count;
        visit(extra);
    } else if (draw.has_reflection || draw.program_alpha_mask) {
        extra.texture = draw.reflection_texture;
        extra.texture_bytes = draw.reflection_bytes;
        extra.texture_byte_count = draw.reflection_byte_count;
        visit(extra);
    }
}

bool prepareAllowed(RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command)
{
    return presenter != nullptr && presenter == active_presenter && command != nullptr &&
        GetCurrentThreadId() == presenter->owner_thread &&
        command->type == RECOMP_D3D_PRESENTER_COMMAND_DRAW;
}

void d3d11_backend_prepare(RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command)
{
    if (!prepareAllowed(presenter, command)) return;
    visitPrepareTextures(command->data.draw, [presenter](const RecompD3dPresenterDrawCommand &draw) {
        prepareTexture(presenter, draw);
    });
}

uint64_t d3d11_backend_prepare_bytes(RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command)
{
    uint64_t bytes = 0;
    if (!prepareAllowed(presenter, command)) return 0;
    visitPrepareTextures(command->data.draw, [presenter, &bytes](const RecompD3dPresenterDrawCommand &draw) {
        if (textureMissing(presenter, draw)) bytes += draw.texture_byte_count;
    });
    return bytes;
}

RecompD3dPresenterError d3d11_backend_submit(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterCommand *command)
{
    if (presenter == nullptr || presenter != active_presenter) {
        return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    }
    if (GetCurrentThreadId() != presenter->owner_thread) {
        return RECOMP_D3D_PRESENTER_WRONG_THREAD;
    }
    if (command == nullptr) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }

    switch (command->type) {
    case RECOMP_D3D_PRESENTER_COMMAND_GAMMA:
        return submitGamma(presenter, command->data.gamma);
    case RECOMP_D3D_PRESENTER_COMMAND_CLEAR:
        return submitClear(presenter, command->data.clear);
    case RECOMP_D3D_PRESENTER_COMMAND_PRESENT:
        return submitPresent(presenter, command->data.present);
    case RECOMP_D3D_PRESENTER_COMMAND_DRAW:
        return submitDraw(presenter, command->data.draw);
    default:
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
}

RecompD3dPresenterError d3d11_backend_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size)
{
    if (presenter == nullptr || presenter != active_presenter) {
        return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    }
    if (GetCurrentThreadId() != presenter->owner_thread) {
        return RECOMP_D3D_PRESENTER_WRONG_THREAD;
    }
    if (base >= 0x80000000u && base < 0x84000000u) base -= 0x80000000u;
    const uint64_t end = static_cast<uint64_t>(base) + size;
    if (size == 0u || end > 0x100000000ull) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    const auto released = [base, end](uint32_t data) {
        return data >= base && data < end;
    };
    const auto colors = presenter->render_targets.size();
    const auto depths = presenter->depth_targets.size();
    bool changed = false;
    for (uint32_t i = 0u; i < presenter->render_targets.size();) {
        RenderTargetEntry &entry = presenter->render_targets[i];
        if (!released(entry.desc.data)) { ++i; continue; }
        releaseCom(entry.sample_view);
        releaseCom(entry.render_view);
        presenter->target_bytes -= entry.bytes;
        entry = presenter->render_targets.back();
        presenter->render_targets.pop_back();
        changed = true;
    }
    for (uint32_t i = 0u; i < presenter->depth_targets.size();) {
        DepthTargetEntry &entry = presenter->depth_targets[i];
        if (!released(entry.desc.data)) { ++i; continue; }
        releaseCom(entry.view);
        presenter->target_bytes -= entry.bytes;
        entry = presenter->depth_targets.back();
        presenter->depth_targets.pop_back();
        changed = true;
    }
    for (uint32_t i = 0u; i < presenter->texture_count; ++i) {
        TextureEntry &entry = presenter->textures[i];
        if (entry.used && released(entry.data)) {
            unindexTexture(presenter, i);
            releaseCom(entry.view);
            entry.used = false;
            changed = true;
        }
    }
    if (changed) {
        // The context also owns bound views. Every subsequent draw rebinds them.
        ID3D11ShaderResourceView *none[2]{};
        presenter->context->PSSetShaderResources(0u, 2u, none);
        presenter->context->OMSetRenderTargets(0u, nullptr, nullptr);
    }
    if (colors != presenter->render_targets.size() || depths != presenter->depth_targets.size()) {
        std::fprintf(stderr,
            "recomp d3d presenter: released storage base=0x%08X size=%u "
            "color=%zu depth=%zu remaining=%zu/%zu\n", base, size,
            colors - presenter->render_targets.size(), depths - presenter->depth_targets.size(),
            presenter->render_targets.size(), presenter->depth_targets.size());
    }
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError d3d11_backend_destroy(
    RecompD3dPresenter **presenter)
{
    if (presenter == nullptr) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    if (*presenter == nullptr || *presenter != active_presenter) {
        return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    }
    if (GetCurrentThreadId() != (*presenter)->owner_thread) {
        return RECOMP_D3D_PRESENTER_WRONG_THREAD;
    }

    RecompD3dPresenter *destroyed = *presenter;
    active_presenter = nullptr;
    *presenter = nullptr;
    destroyed->precompile_stop = true;
    if (destroyed->precompile.joinable()) destroyed->precompile.join();
    releasePresenter(destroyed);
    delete destroyed;
    return RECOMP_D3D_PRESENTER_OK;
}

void d3d11_backend_set_immediate_present(bool enabled)
{
    immediate_present = enabled;
}

/* Split presentation paces itself: a third buffer absorbs a late compositor
   frame instead of blocking the next Present. */
void d3d11_backend_set_split_presentation(bool enabled)
{
    split_presentation = enabled;
}

void d3d11_backend_verify_replay(RecompD3dPresenter *presenter, uint32_t frame, bool replay)
{
    presenter->replay_verify_frame = frame;
    presenter->replay_verify_second = replay;
}


void d3d11_backend_report_draw_textures(void)
{
    for (uint32_t format = 0u; format < 256u; ++format) {
        const uint32_t sampled = draw_texture_tally.textured[format];
        const uint32_t rejected = draw_texture_tally.rejected[format];

        if (sampled == 0u && rejected == 0u) {
            continue;
        }
        std::fprintf(
            stderr,
            "recomp d3d presenter: draw texture fmt=0x%02x sampled=%u "
            "rejected=%u\n",
            format,
            sampled,
            rejected);
    }
    std::fprintf(
        stderr,
        "recomp d3d presenter: draw texture untextured=%u cached=%u\n",
        draw_texture_tally.untextured,
        active_presenter != nullptr ? active_presenter->texture_count : 0u);
}
