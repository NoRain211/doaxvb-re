#include "d3d_presenter_d3d11_backend.h"
#include "d3d_draw_model.h"
#include "d3d_vblank.h"
#include "d3d_vertex_program.h"
#include "overlay_d3d11.h"
#include "screenshot.h"
#include "texture_replacement.h"
#include "d3d_smooth_normals.h"
#ifdef RECOMP_RTGI
#include "d3d_rtgi.h"
#endif
#include "d3d_tuning.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_5.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <atomic>
#include <mutex>
#include <new>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <list>
#include <string>

#include "d3d_sun_shadow.h"
#include "d3d_gbuffer.h"
#include "dlss_bridge.h"

#ifdef RECOMP_SMAA
#include "AreaTex.h"
#include "SearchTex.h"
static const char kSmaaSource[] = {
#include "smaa_hlsl.inc"
};
#endif

#ifdef RECOMP_FSR1
// AMD FidelityFX FSR 1.0 (third_party/fsr1, MIT): the CPU half sets the
// shader constants; the GPU half is embedded and compiled at startup.
#include <math.h>
#include <stdlib.h>
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"
#undef A_CPU
static const char kFfxASource[] = {
#include "ffx_a_hlsl.inc"
};
static const char kFfxFsr1Source[] = {
#include "ffx_fsr1_hlsl.inc"
};
#endif

namespace {

constexpr wchar_t kWindowClassName[] = L"DOAXBVRecompPresenterWindow";
constexpr wchar_t kWindowTitle[] = L"DOAXBV Recomp";

// Float indices of taa_jitter, previous_wvp and motion_flags in the draw
// constant buffer (kDrawShaderPrologue), and its size.
constexpr size_t kJitterConstant = 140u + 192u * 4u + 336u + 76u + 16u;
constexpr size_t kPreviousWvpConstant = kJitterConstant + 4u;
constexpr size_t kMotionFlagsConstant = kPreviousWvpConstant + 64u;
constexpr size_t kDrawConstantFloats = kMotionFlagsConstant + 4u;

// Set before any draw pipeline compiles: every pipeline then carries object motion.
bool object_motion_shaders = false;

// Legacy keeps the original fixed 1x window or screen-sized borderless popup.
enum class WindowMode { Legacy, Windowed, Borderless };

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
    "    float4 light_specular[8];\n"
    "    float4 light_view;\n"
    "    float4 fog_color;\n"
    "    float4 fog_params; // start, end, density, mode\n"
    "    float4 fog_flags; // enabled, Z depth, range, visualization\n"
    "    row_major float4x4 fog_world_view[4];\n"
    "    float4 combiner[4];\n"
    "    float4 taa_jitter; // xy: clip-space offset of 3D scene draws, else zero\n"
    "    row_major float4x4 previous_wvp[4]; // last frame's wvp[], for object motion\n"
    "    float4 motion_flags; // x: write object motion; zw: scene size in pixels\n"
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
    "SamplerState mask_sampler : register(s1);\n"
    /* Xbox D3DTA selectors 0-4 with COMPLEMENT (0x10) and ALPHAREPLICATE
       (0x20); absent specular is black. */
    "float4 ff_arg(int a, float4 cur, float4 dif, float4 tex) {\n"
    "    int s = a & 15;\n"
    "    float4 v = s == 0 ? dif : s == 2 ? tex : s == 3 ? texture_factor\n"
    "        : s == 4 ? float4(0, 0, 0, 0) : cur;\n"
    "    if (a & 0x20) v = v.aaaa;\n"
    "    if (a & 0x10) v = 1.0f - v;\n"
    "    return v;\n"
    "}\n"
    /* Xbox values differ from PC D3D8: BLENDCURRENTALPHA=13,
       DOTPRODUCT3=22, MULTIPLYADD=23, LERP=24. See Cxbx XbD3D8Types.h:
       https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/XbD3D8Types.h
       PREMODULATE is declined by the adapter. */
    "float4 ff_op(float4 o, float4 cur, float4 dif, float4 tex) {\n"
    "    float4 a0 = ff_arg((int)o.y, cur, dif, tex);\n"
    "    float4 a1 = ff_arg((int)o.z, cur, dif, tex);\n"
    "    float4 a2 = ff_arg((int)o.w, cur, dif, tex);\n"
    "    float4 r = cur;\n"
    "    switch ((int)o.x) {\n"
    "    case 2: r = a1; break;\n"
    "    case 3: r = a2; break;\n"
    "    case 4: r = a1 * a2; break;\n"
    "    case 5: r = 2.0f * a1 * a2; break;\n"
    "    case 6: r = 4.0f * a1 * a2; break;\n"
    "    case 7: r = a1 + a2; break;\n"
    "    case 8: r = a1 + a2 - 0.5f; break;\n"
    "    case 9: r = 2.0f * (a1 + a2 - 0.5f); break;\n"
    "    case 10: r = a1 - a2; break;\n"
    "    case 11: r = a1 + a2 - a1 * a2; break;\n"
    "    case 12: r = lerp(a2, a1, dif.a); break;\n"
    "    case 13: r = lerp(a2, a1, cur.a); break;\n"
    "    case 14: r = lerp(a2, a1, tex.a); break;\n"
    "    case 15: r = lerp(a2, a1, texture_factor.a); break;\n"
    "    case 16: r = a1 + a2 * (1.0f - tex.a); break;\n"
    "    case 18: r = a1 + a1.a * a2; break;\n"
    "    case 19: r = a1 * a2 + a1.a; break;\n"
    "    case 20: r = a1 + (1.0f - a1.a) * a2; break;\n"
    "    case 21: r = (1.0f - a1) * a2 + a1.a; break;\n"
    "    case 22: r = saturate(4.0f * dot(a1.rgb - 0.5f, a2.rgb - 0.5f)).xxxx; break;\n"
    "    case 23: r = a0 + a1 * a2; break;\n"
    "    case 24: r = lerp(a2, a1, a0); break;\n"
    "    }\n"
    "    return saturate(r);\n"
    "}\n"
    "float4 ff_combine(float4 dif, float4 t0, float4 t1) {\n"
    "    float4 cur = dif;\n"
    "    [unroll] for (int s = 0; s < 2; ++s) {\n"
    "        float4 c = combiner[s * 2], a = combiner[s * 2 + 1];\n"
    "        if (c.x < 1.5f) break;\n"
    "        float4 tex = s == 0 ? t0 : t1;\n"
    "        float4 next = ff_op(c, cur, dif, tex);\n"
    "        if (a.x > 1.5f) next.a = ff_op(a, cur, dif, tex).a; else next.a = cur.a;\n"
    "        cur = next;\n"
    "    }\n"
    "    return cur;\n"
    "}\n";

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
        "    float3 lit_specular : TEXCOORD15;\n"
        "    float2 texcoord : TEXCOORD0;\n"
        "    float2 reflection_coord : TEXCOORD4;\n"
        "%s"
        "};\n"
        "VSOut vs_main(VSIn input) {\n"
        "    VSOut output; output.lit_specular=0;\n"
        "%s"
        "    output.position.xy += taa_jitter.xy * output.position.w;\n"
        "%s"
        "%s"
        "%s"
        "    output.reflection_coord = float2(0, 0);\n"
        "%s"
        "    return output;\n"
        "}\n"
        "float4 ps_main(VSOut input) : SV_TARGET {\n"
        "    float4 shaded = input.color;\n"
        "    if (reflection_flags.x > 2.5f) {\n"
        "        float4 t0 = guest_texture.Sample(guest_sampler, input.texcoord * texture_flags.xy);\n"
        "        if (lighting_flags.y > 0.5f) t0.rgb = 1.0f;\n"
        "        float4 t1 = alpha_mask.Sample(mask_sampler, input.reflection_coord * lighting_flags.zw);\n"
        "        if (reflection_flags.y > 0.5f) t1.rgb = 1.0f;\n"
        "        float4 dif = input.color;\n"
        "        if (directional_flags.x > 0.5f) dif.a *= blend_flags.z;\n"
        "        shaded = ff_combine(dif, t0, t1);\n"
        "    } else if (reflection_flags.x > 0.5f) {\n"
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
        "    if (directional_flags.x > 0.5f && light_view.w > 0) shaded.rgb += input.lit_specular;\n"
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
        "#ifdef EXPLICIT_COVERAGE\n"
        "        if (draw_flags.y > 1.5f) {\n"
        /* Coverage is independent of stored alpha, including flat hard passes. */
        "            float threshold = (ref + (func == 4 ? 0.5f : -0.5f)) / 255.0f;\n"
        "            float width = fwidth(shaded.a);\n"
        "            float coverage = saturate((shaded.a - threshold) / max(width, 1e-4f) + 0.5f);\n"
        "            coverage = width <= 1e-4f || (func == 6 && (ref == 0 || ref == 255))\n"
        "                ? (alpha_pass ? 1.0f : 0.0f)\n"
        "                : (shaded.a > 0 && (func != 4 || ref < 255) ? coverage : 0);\n"
        "            uint count = (uint)round(coverage * draw_flags.y);\n"
        "            sample_coverage = count >= 32 ? 0xffffffffu : (1u << count) - 1u;\n"
        "        } else\n"
        "#endif\n"
        "        if (!alpha_pass) discard;\n"
        "    }\n"
        "    if (fog_flags.x > 0.5f) {\n"
        "        float depth = fog_flags.y > 0.5f && fog_flags.z < 0.5f ? abs(input.position.z) : input.fog_depth;\n"
        "        float factor = fog_params.w == 0 ? saturate(input.fog_factor) : fogFactor(depth);\n"
        "        shaded.rgb = fog_flags.w > 0.5f ? factor.xxx : lerp(fog_color.rgb, shaded.rgb, factor);\n"
        "    }\n"
        /* Dither before the first UNORM quantization; post-process input is already 8-bit. */
        "    if (directional_flags.w > 0.5f) {\n"
        "        uint2 p = (uint2)input.position.xy;\n"
        "        uint b = ((p.x ^ p.y) & 1) * 8 + (p.y & 1) * 4\n"
        "            + (((p.x ^ p.y) >> 1) & 1) * 2 + ((p.y >> 1) & 1);\n"
        "        shaded.rgb += ((b + 0.5f) / 16.0f - 0.5f) / 255.0f;\n"
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
                ? "    if (reflection_flags.x > 2.5f) {\n"
                  "        output.reflection_coord = mul(float4(input.texcoord, 1, 0), reflection_transform).xy;\n"
                  "    } else if (reflection_flags.x > 1.5f) {\n"
                  "        output.reflection_coord = input.texcoord;\n"
                  "    } else if (reflection_flags.x > 0.5f) {\n"
                  "        float3 eye = mul(float4(input.position, 1), reflection_world_view).xyz;\n"
                  "        float3 n = mul(float4(input.normal, 0), reflection_normal).xyz;\n"
                  "        if (reflection_flags.y > 0.5f) n = normalize(n);\n"
                  "        float3 r = reflect(normalize(eye), n);\n"
                  "        output.reflection_coord = mul(float4(r, 1), reflection_transform).xy;\n"
                  "    }\n"
                : "    if (reflection_flags.x > 2.5f) {\n"
                  "        output.reflection_coord = mul(float4(input.texcoord, 1, 0), reflection_transform).xy;\n"
                  "    } else if (reflection_flags.x > 1.5f) {\n"
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

struct SmoothNormalsEntry {
    uint64_t slot, used;
    uint32_t address, vertex_count, first, fvf, stride, primitive;
    // The referenced vertex range before and after smoothing.
    std::vector<uint8_t> source, vertices;
    std::vector<uint16_t> indices;
    size_t bytes() const { return source.size()+vertices.size()+indices.size()*2u; }
};

struct SmoothNormalsStats {
    uint64_t hits = 0, misses = 0, evictions = 0;
    double total_ms = 0, rebuild_ms = 0;
};

struct SmoothNormalsCache {
    std::list<SmoothNormalsEntry> entries; // Least recently used first.
    std::unordered_multimap<uint64_t, std::list<SmoothNormalsEntry>::iterator> slots;
    size_t bytes = 0;
    uint64_t clock = 0;
    SmoothNormalsStats stats, reported;
    double max_rebuild_ms = 0; // Since the last performance line.
};

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
    bool alpha_to_coverage;
    bool normal_write;
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
    SunOverlay sun_overlay = SunOverlay::None;
    TextureIdentity identity;
    ID3D11ShaderResourceView *view;
};

/* A guest buffer can be refilled with another texture of the same size and
   format without being freed (item previews reuse theirs), so a hit must also
   match the texels.
   ponytail: samples 128 words; a rewrite missing all of them stays stale.
   Hash the whole span if that is ever observed. */
uint64_t textureFingerprint(const void *bytes, uint32_t count)
{
    return recomp_d3d_texture_fingerprint(bytes, count);
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
    SunPalmTarget sun_palm;
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
    SunShadow sun;
    GBuffer gbuffer;
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
    bool rtgi_requested = false; // RECOMP_D3D_RTGI=1: ray-traced GI when available
#ifdef RECOMP_RTGI
    D3dRtgi *rtgi = nullptr;
    bool rtgi_pending = false;  // 3D draws recorded since the last GI pass
    bool rtgi_applied = false;  // GI already lit this frame; later draws skip it
#endif
    ID3D11ShaderResourceView *depth_sample = nullptr; // main depth, RTGI or G-buffer
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
    /* RECOMP_D3D_WINDOW_MODE=windowed|borderless: the swap chain matches the
       window and the main-sized output is scaled into it, letterboxed to the
       game's aspect (the VRR resources above). Alt+Enter switches modes. */
    WindowMode window_mode = WindowMode::Legacy;
    uint32_t window_height_request = 0u; // RECOMP_D3D_WINDOW_HEIGHT: windowed client height
    WINDOWPLACEMENT windowed_placement{}; // restored when leaving borderless
    bool resize_pending = false;
    UINT swap_flags = 0u;
    // RECOMP_D3D_UPSCALE=fsr: FSR 1.0 EASU upscale and RCAS sharpen into the window.
    bool fsr_requested = false;
    float fsr_sharpness = 0.2f;  // RECOMP_D3D_SHARPNESS 0..1, mapped to RCAS stops
    ID3D11VertexShader *fsr_vs = nullptr;
    ID3D11PixelShader *fsr_easu = nullptr;
    ID3D11PixelShader *fsr_rcas = nullptr;
    ID3D11Buffer *fsr_constants = nullptr;
    ID3D11RenderTargetView *fsr_target = nullptr; // EASU output at the letterbox size
    ID3D11ShaderResourceView *fsr_source = nullptr;
    uint32_t fsr_width = 0u, fsr_height = 0u;
    uint32_t anisotropy = 0u;    // RECOMP_D3D_ANISOTROPY: force anisotropic filtering, 2..16
    /* Temporal upscaler inputs (docs/motion-vectors.md). Camera motion comes
       from scene depth reprojected with the previous frame's camera. */
    bool motion_requested = false; // RECOMP_D3D_MOTION_VECTORS=1, or the debug view
    bool motion_debug = false;     // RECOMP_D3D_MOTION_DEBUG=1: show vectors as colour
    bool jitter_enabled = false;   // RECOMP_D3D_JITTER=1: sub-pixel offset per frame
    uint32_t jitter_phases = 8u;
    uint32_t jitter_index = 0u;
    float jitter[2]{};             // this frame's offset, clip units
    float jitter_pixels[2]{};      // the same offset, render pixels
    float camera[16]{};            // this frame's scene view-projection
    float previous_camera[16]{};
    uint32_t camera_weight = 0u;   // indices of the draw that named the camera
    bool has_previous_camera = false;
    bool scene_pending = false;    // 3D scene drawn since the last motion pass
    bool motion_done = false;
    ID3D11VertexShader *motion_vs = nullptr;
    ID3D11PixelShader *motion_ps = nullptr;
    ID3D11PixelShader *motion_debug_ps = nullptr;
    ID3D11BlendState *motion_debug_blend = nullptr;
    ID3D11Buffer *motion_constants = nullptr;
    ID3D11RenderTargetView *motion_target = nullptr; // R16G16_FLOAT, render pixels
    ID3D11ShaderResourceView *motion_view = nullptr;
    /* Object vectors: scene draws write rg = pixels moved, b = depth, a = 1
       here; the motion pass keeps them where that surface is still visible. */
    ID3D11RenderTargetView *object_motion_target = nullptr; // R16G16B16A16_FLOAT, scene size
    ID3D11ShaderResourceView *object_motion_view = nullptr;
    struct ObjectHistory {
        float world[4][16];
        float view_projection[16];
        uint32_t matrices;
    };
    /* Keyed by mesh. A mesh drawn several times (foliage) matches each copy to
       last frame's nearest by world position: draw order changes as the game
       sorts by distance, so order alone pairs the wrong copies. */
    std::unordered_map<uint64_t, std::vector<ObjectHistory>> objects_previous, objects_current;
    size_t objects_recorded = 0u;
    /* RECOMP_D3D_DLSS (docs/dlss.md): the 3D scene renders into a smaller
       target set, DLSS reconstructs it into the full-size set, and the HUD
       draws there. The fields above always name the set in use; `other`
       holds the set not in use. */
    struct MainTargets {
        ID3D11RenderTargetView *color = nullptr;
        ID3D11Texture2D *depth_texture = nullptr;
        ID3D11DepthStencilView *depth_view = nullptr;
        ID3D11ShaderResourceView *depth_sample = nullptr;
        ID3D11Texture2D *back_copy = nullptr;
        ID3D11ShaderResourceView *back_sample = nullptr;
    } other;
    int dlss_mode = -1;            // DlssBridgeMode, or -1 for off
    bool dlss_active = false;
    bool scene_phase = false;      // drawing the 3D scene into the DLSS input set
    bool dlss_reset = true;
    uint32_t dlss_render_width = 0u, dlss_render_height = 0u;
    HMODULE dlss_module = nullptr;
    DlssBridgeEvaluate dlss_evaluate = nullptr;
    DlssBridgeShutdown dlss_shutdown = nullptr;
    DlssBridgeError dlss_error = nullptr;
    ID3D11ShaderResourceView *scene_color = nullptr;  // scene-set colour, for the fallback copy
    ID3D11Texture2D *dlss_output = nullptr;           // full-size RGBA8 with unordered access
    ID3D11ShaderResourceView *dlss_output_view = nullptr;
    ID3D11VertexShader *dlss_copy_vs = nullptr;
    ID3D11PixelShader *dlss_copy_ps = nullptr;
    ID3D11SamplerState *dlss_sampler = nullptr;
    double dlss_last_ms = 0.0;
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
    SmoothNormalsCache smooth_normals;
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
    TextureReplacements replacements;
    size_t texture_identity_bytes = 0;
    uint32_t texture_count = 0u;
    uint32_t next_texture_slot = 0u;
    std::vector<RenderTargetEntry> render_targets;
    std::vector<DepthTargetEntry> depth_targets;
    uint64_t target_bytes = 0u;
    ID3D11SamplerState *draw_sampler = nullptr;
    ID3D11SamplerState *address_samplers[2][4][4]{};
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
    UINT sync_rate = 60u;
    std::chrono::steady_clock::time_point next_refresh_check{};
    /* Render cost per frame, from its first command to the output pass:
       worker CPU time and GPU timestamps, read three frames late without
       stalling. */
    struct GpuTimer { ID3D11Query *disjoint = nullptr, *begin = nullptr, *end = nullptr; } gpu_timers[4];
    unsigned gpu_timer_next = 0u;
    bool frame_timing = false;
    double frame_start_ms = 0.0, gpu_frame_ms = 0.0, cost_cpu_ms = 0.0, cost_gpu_ms = 0.0;
    unsigned cost_frames = 0u, cost_gpu_frames = 0u;
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
static bool unlocked_present;

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
/* While DLSS's 3D scene is drawn, the main target set is the smaller input. */
uint32_t mainHeight(const RecompD3dPresenter *presenter)
{
    if (presenter->scene_phase) return presenter->dlss_render_height;
    return static_cast<uint32_t>(presenter->config.height * presenter->scale + 0.5f);
}

uint32_t mainWidth(const RecompD3dPresenter *presenter)
{
    if (presenter->scene_phase) return presenter->dlss_render_width;
    return presenter->scale == 1.0f
        ? presenter->config.width : presentClientWidth(presenter, mainHeight(presenter));
}

// The 3D scene's size: the DLSS input while DLSS is on, else the main target.
uint32_t sceneWidth(const RecompD3dPresenter *presenter)
{
    return presenter->dlss_active ? presenter->dlss_render_width : mainWidth(presenter);
}

uint32_t sceneHeight(const RecompD3dPresenter *presenter)
{
    return presenter->dlss_active ? presenter->dlss_render_height : mainHeight(presenter);
}

/* Beyond the screen the swap chain downsamples into the largest window at the
   presentation aspect that fits inside it on both axes. */
uint32_t fitWindowHeight(
    const RecompD3dPresenter *presenter, uint64_t screen_width, uint32_t screen_height)
{
    const uint32_t width_fit = static_cast<uint32_t>(presenter->widescreen
        ? screen_width * 9u / 16u : screen_width * 3u / 4u);
    return (std::min)((std::min)(mainHeight(presenter), screen_height), width_fit);
}

uint32_t windowHeight(const RecompD3dPresenter *presenter)
{
    return presenter->scale == 1.0f ? presenter->config.height : fitWindowHeight(presenter,
        static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN)),
        static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN)));
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

// The swap chain is window-sized and the presenter scales the frame into it.
bool outputScaled(const RecompD3dPresenter *presenter)
{
    return presenter->window_mode != WindowMode::Legacy || vrrScaled(presenter);
}

constexpr DWORD kWindowedStyle = WS_OVERLAPPEDWINDOW;

/* Fills the window's monitor without a frame, or restores the framed window.
   Flip-model swap chains on a borderless window still get direct flip. */
void setBorderless(RecompD3dPresenter *presenter, bool borderless)
{
    HWND window = presenter->window;
    const LONG_PTR visible = GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE;
    if (borderless) {
        presenter->windowed_placement.length = sizeof presenter->windowed_placement;
        if (presenter->window_mode == WindowMode::Windowed) {
            GetWindowPlacement(window, &presenter->windowed_placement);
        }
        MONITORINFO monitor{};
        monitor.cbSize = sizeof monitor;
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | visible);
        SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right - monitor.rcMonitor.left,
            monitor.rcMonitor.bottom - monitor.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER | (visible ? SWP_SHOWWINDOW : 0u));
        presenter->window_mode = WindowMode::Borderless;
    } else {
        SetWindowLongPtrW(window, GWL_STYLE, kWindowedStyle | visible);
        if (presenter->windowed_placement.length != 0u) {
            WINDOWPLACEMENT placement = presenter->windowed_placement;
            if (!visible) placement.showCmd = SW_HIDE;
            SetWindowPlacement(window, &placement);
        }
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        presenter->window_mode = WindowMode::Windowed;
    }
    presenter->resize_pending = true;
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
    auto *scaled = reinterpret_cast<RecompD3dPresenter *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (scaled != nullptr && scaled->window_mode != WindowMode::Legacy) {
        switch (message) {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) scaled->resize_pending = true;
            break;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {320, 240};
            return 0;
        case WM_SYSKEYDOWN:
            // Alt+Enter: the presenter's own borderless switch; DXGI's is disabled.
            if (wparam == VK_RETURN && (lparam & (1 << 29)) && !(lparam & (1 << 30))) {
                setBorderless(scaled, scaled->window_mode != WindowMode::Borderless);
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (wparam == VK_RETURN) return 0;  // no beep for Alt+Enter
            break;
        case WM_MENUCHAR:
            return MAKELRESULT(0, MNC_CLOSE);
        case WM_SETCURSOR:
            if (scaled->window_mode == WindowMode::Borderless && LOWORD(lparam) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        default:
            break;
        }
    }
    // Shift+F12 is the screenshot key (screenshot.h).
    if (message == WM_KEYDOWN && wparam == VK_F12 && !(lparam & (1u << 30)) &&
        (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
        auto *presenter = reinterpret_cast<RecompD3dPresenter *>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (presenter && presenter->replacements.enabled()) {
            presenter->replacements.request((GetKeyState(VK_CONTROL) & 0x8000) != 0);
            return 0;
        }
    }
    if (message == WM_CLOSE) {
        auto *presenter = reinterpret_cast<RecompD3dPresenter *>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (presenter != nullptr) {
            presenter->close_requested = true;
        }
        recomp_tuning.paused = false;   // let a paused game reach its next present and exit
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

void releaseDlss(RecompD3dPresenter *presenter);

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
    presenter->replacements.clear();
    if (presenter->context != nullptr) {
        presenter->context->OMSetRenderTargets(0u, nullptr, nullptr);
        presenter->context->ClearState();
        presenter->context->Flush();
    }
    releaseDlss(presenter);  // NGX needs the device it was created on
    // Keep the opt-in across the hardware-to-WARP retry; sunCreate failure still disables it.
    const bool sun_enabled = presenter->sun.enabled;
    sunRelease(presenter->sun);
    presenter->sun.enabled = sun_enabled;
    presenter->gbuffer.release();
    for (auto &timer : presenter->gpu_timers) {
        releaseCom(timer.disjoint);
        releaseCom(timer.begin);
        releaseCom(timer.end);
    }
    releaseCom(presenter->present_target_view);
    releaseCom(presenter->vrr_target_view);
    releaseCom(presenter->vrr_source);
    releaseCom(presenter->vrr_vs);
    releaseCom(presenter->vrr_ps);
    releaseCom(presenter->vrr_sampler);
    releaseCom(presenter->fsr_vs);
    releaseCom(presenter->fsr_easu);
    releaseCom(presenter->fsr_rcas);
    releaseCom(presenter->fsr_constants);
    releaseCom(presenter->fsr_target);
    releaseCom(presenter->fsr_source);
    presenter->fsr_width = presenter->fsr_height = 0u;
    releaseCom(presenter->motion_vs);
    releaseCom(presenter->motion_ps);
    releaseCom(presenter->motion_debug_ps);
    releaseCom(presenter->motion_debug_blend);
    releaseCom(presenter->motion_constants);
    releaseCom(presenter->motion_target);
    releaseCom(presenter->motion_view);
    releaseCom(presenter->object_motion_target);
    releaseCom(presenter->object_motion_view);
    presenter->objects_previous.clear();
    presenter->objects_current.clear();
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
        entry.identity = {};
        entry.used = false;
    }
    presenter->texture_identity_bytes = 0;
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
    for (auto &filter : presenter->address_samplers) {
        for (auto &row : filter) {
            for (auto &sampler : row) releaseCom(sampler);
        }
    }
    releaseCom(presenter->filter_sampler);
    releaseCom(presenter->program_mask_sampler);
    presenter->draw_vertex_capacity = 0u;
    presenter->draw_index_capacity = 0u;
    presenter->draw_vertex_used = 0u;
    presenter->draw_index_used = 0u;
    presenter->draw_shared_ready = false;
#ifdef RECOMP_RTGI
    d3d_rtgi_destroy(presenter->rtgi);
    presenter->rtgi = nullptr;
#endif
    releaseCom(presenter->depth_sample);
    releaseCom(presenter->depth_view);
    releaseCom(presenter->depth_texture);
    releaseCom(presenter->render_target_view);
    releaseCom(presenter->back_buffer_sample);
    releaseCom(presenter->back_buffer_copy);
    releaseCom(presenter->target_copy_vs);
    releaseCom(presenter->target_copy_ps);
    releaseCom(presenter->front_buffer_sample);
    releaseCom(presenter->front_buffer_copy);
    // The overlay's ImGui backend holds the device; it starts again on the next.
    recomp_overlay_d3d11_shutdown();
    releaseCom(presenter->swap_chain);
    releaseCom(presenter->context);
    releaseCom(presenter->device);
}

void flushFrameDumps(RecompD3dPresenter *presenter);

// Saves the window's back buffer, before the overlay is drawn on it, through
// a CPU-readable copy. Only a requested screenshot pays for the wait.
void saveScreenshot(RecompD3dPresenter *presenter)
{
    ID3D11Texture2D *back_buffer = nullptr;
    presenter->swap_chain->GetBuffer(0u, IID_PPV_ARGS(&back_buffer));
    if (back_buffer == nullptr) return;
    D3D11_TEXTURE2D_DESC desc{};
    back_buffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0u;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0u;
    ID3D11Texture2D *staging = nullptr;
    if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM && desc.SampleDesc.Count == 1u &&
        SUCCEEDED(presenter->device->CreateTexture2D(&desc, nullptr, &staging))) {
        presenter->context->CopyResource(staging, back_buffer);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(presenter->context->Map(staging, 0u, D3D11_MAP_READ, 0u, &mapped))) {
            recomp_screenshot_save(static_cast<const uint8_t *>(mapped.pData),
                desc.Width, desc.Height, mapped.RowPitch);
            presenter->context->Unmap(staging, 0u);
        }
    }
    releaseCom(staging);
    releaseCom(back_buffer);
}

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
    if (presenter->dlss_module != nullptr) FreeLibrary(presenter->dlss_module);
    presenter->dlss_module = nullptr;
}

bool registerWindowClass(RecompD3dPresenter *presenter);

/* Windowed and borderless modes start from a framed window centred on the
   primary work area; the client keeps the game's aspect at the requested
   height, or 85% of the work area by default. */
bool createScaledWindow(RecompD3dPresenter *presenter)
{
    SetProcessDPIAware();
    if (!registerWindowClass(presenter)) return false;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0u, &work, 0u);
    const LONG work_width = work.right - work.left, work_height = work.bottom - work.top;
    uint32_t height = presenter->window_height_request != 0u
        ? presenter->window_height_request : static_cast<uint32_t>(work_height * 0.85);
    height = (std::max)(240u, (std::min)(height, static_cast<uint32_t>(work_height)));
    uint32_t width = presentClientWidth(presenter, height);
    if (width > static_cast<uint32_t>(work_width)) {
        width = static_cast<uint32_t>(work_width);
        height = presenter->widescreen ? width * 9u / 16u : width * 3u / 4u;
    }
    RECT rect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRectEx(&rect, kWindowedStyle, FALSE, 0u);
    const LONG outer_width = rect.right - rect.left, outer_height = rect.bottom - rect.top;
    presenter->window = CreateWindowExW(0u, kWindowClassName, kWindowTitle, kWindowedStyle,
        work.left + (std::max)(0L, (work_width - outer_width) / 2),
        work.top + (std::max)(0L, (work_height - outer_height) / 2),
        outer_width, outer_height, nullptr, nullptr, presenter->instance, presenter);
    if (presenter->window == nullptr) return false;
    if (presenter->window_mode == WindowMode::Borderless) {
        presenter->window_mode = WindowMode::Windowed;  // remembers this placement
        setBorderless(presenter, true);
    }
    presenter->resize_pending = false;  // the swap chain is created at this size
    return true;
}

bool registerWindowClass(RecompD3dPresenter *presenter)
{
    WNDCLASSEXW window_class{};
    presenter->instance = GetModuleHandleW(nullptr);
    window_class.cbSize = sizeof window_class;
    window_class.lpfnWndProc = presenterWindowProc;
    window_class.hInstance = presenter->instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.hIcon = static_cast<HICON>(LoadImageW(presenter->instance, MAKEINTRESOURCEW(1),
        IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(presenter->instance, MAKEINTRESOURCEW(1),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    window_class.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&window_class) == 0u) return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    presenter->owns_window_class = true;
    return true;
}

bool createWindow(RecompD3dPresenter *presenter)
{
    if (presenter->window_mode != WindowMode::Legacy) return createScaledWindow(presenter);
    /* Screen metrics and the borderless window must be physical pixels; a
       DPI-virtualized window is downsampled and then stretched by DWM. */
    if (presenter->scale > 1.0f) SetProcessDPIAware();
    WNDCLASSEXW window_class{};
    const uint32_t client_width = presentClientWidth(presenter, windowHeight(presenter));
    RECT window_rect = {
        0,
        0,
        static_cast<LONG>(client_width),
        static_cast<LONG>(windowHeight(presenter)),
    };
    /* A scaled window fills the screen; decorations would not fit. The 1x
       window keeps a fixed size because a resized client stretches the image. */
    const bool borderless = presenter->scale > 1.0f;
    const DWORD style = borderless
        ? WS_POPUP : WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

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
    /* shortcut: a screen wider or taller than the image shows the desktop
       around it; draw black bars from a screen-sized window if that bothers. */
    const int x = borderless
        ? (GetSystemMetrics(SM_CXSCREEN) - static_cast<int>(client_width)) / 2 : CW_USEDEFAULT;
    const int y = borderless
        ? (GetSystemMetrics(SM_CYSCREEN) - static_cast<int>(windowHeight(presenter))) / 2 : CW_USEDEFAULT;

    presenter->window = CreateWindowExW(
        0u,
        kWindowClassName,
        kWindowTitle,
        style,
        x,
        y,
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

    RECT client{};
    if (presenter->window_mode != WindowMode::Legacy && GetClientRect(presenter->window, &client)) {
        swap_chain_desc.BufferDesc.Width = static_cast<UINT>((std::max)(1L, client.right - client.left));
        swap_chain_desc.BufferDesc.Height = static_cast<UINT>((std::max)(1L, client.bottom - client.top));
    } else {
        swap_chain_desc.BufferDesc.Width = vrrScaled(presenter)
            ? presentClientWidth(presenter, windowHeight(presenter)) : mainWidth(presenter);
        swap_chain_desc.BufferDesc.Height = vrrScaled(presenter)
            ? windowHeight(presenter) : mainHeight(presenter);
    }
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
    presenter->swap_flags = swap_chain_desc.Flags;  // ResizeBuffers must repeat them

    /* Hybrid laptops default to the integrated GPU, which also lacks DLSS.
       Ask DXGI for the high-performance adapter unless RECOMP_D3D_GPU=integrated. */
    IDXGIAdapter1 *adapter = nullptr;
    if (driver_type == D3D_DRIVER_TYPE_HARDWARE) {
        const char *gpu = std::getenv("RECOMP_D3D_GPU");
        const DXGI_GPU_PREFERENCE preference = gpu != nullptr && std::strcmp(gpu, "integrated") == 0
            ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
        IDXGIFactory6 *factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            if (FAILED(factory->EnumAdapterByGpuPreference(0u, preference, IID_PPV_ARGS(&adapter)))) adapter = nullptr;
            factory->Release();
        }
        DXGI_ADAPTER_DESC1 adapter_desc{};
        if (adapter != nullptr && SUCCEEDED(adapter->GetDesc1(&adapter_desc))) {
            std::fprintf(stderr, "recomp d3d presenter: adapter %ls\n", adapter_desc.Description);
        }
    }
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        adapter,
        adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : driver_type,
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
    if (adapter != nullptr) adapter->Release();
    // Flip chains use the per-chain limit; immediate presents retain the default.
    if (SUCCEEDED(result) && !immediate) {
        IDXGISwapChain2 *chain = nullptr;
        result = presenter->swap_chain->QueryInterface(IID_PPV_ARGS(&chain));
        if (SUCCEEDED(result)) result = chain->SetMaximumFrameLatency(1u);
        releaseCom(chain);
    }
    // DXGI's Alt+Enter would enter exclusive fullscreen; the window proc owns it.
    if (SUCCEEDED(result) && presenter->window_mode != WindowMode::Legacy) {
        IDXGIFactory *factory = nullptr;
        if (SUCCEEDED(presenter->swap_chain->GetParent(IID_PPV_ARGS(&factory)))) {
            factory->MakeWindowAssociation(presenter->window, DXGI_MWA_NO_ALT_ENTER);
            factory->Release();
        }
    }
    return result;
}

HRESULT createMainDepth(RecompD3dPresenter *presenter)
{
    // Keep the D24S8 DSV; only opt-in consumers need a typeless resource and SRV.
    HRESULT result = S_OK;
    for (const bool readable_depth : {presenter->rtgi_requested || presenter->gbuffer.enabled ||
             presenter->motion_requested, false}) {
        releaseCom(presenter->depth_sample);
        releaseCom(presenter->depth_view);
        releaseCom(presenter->depth_texture);
        D3D11_TEXTURE2D_DESC depth_desc{};
        depth_desc.Width = mainWidth(presenter);
        depth_desc.Height = mainHeight(presenter);
        depth_desc.MipLevels = 1u;
        depth_desc.ArraySize = 1u;
        depth_desc.Format = readable_depth
            ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count = presenter->msaa;
        depth_desc.Usage = D3D11_USAGE_DEFAULT;
        depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL |
            (readable_depth ? D3D11_BIND_SHADER_RESOURCE : 0u);
        result = presenter->device->CreateTexture2D(
            &depth_desc, nullptr, &presenter->depth_texture);
        D3D11_DEPTH_STENCIL_VIEW_DESC depth_view_desc{};
        depth_view_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_view_desc.ViewDimension = presenter->msaa > 1u
            ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        if (SUCCEEDED(result)) {
            result = presenter->device->CreateDepthStencilView(
                presenter->depth_texture, readable_depth ? &depth_view_desc : nullptr,
                &presenter->depth_view);
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sample_desc{};
        sample_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sample_desc.ViewDimension = presenter->msaa > 1u
            ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        sample_desc.Texture2D.MipLevels = 1u; // Ignored for Texture2DMS.
        if (SUCCEEDED(result) && readable_depth) {
            result = presenter->device->CreateShaderResourceView(
                presenter->depth_texture, &sample_desc, &presenter->depth_sample);
        }
        if (SUCCEEDED(result) || !readable_depth) break;
        std::fprintf(stderr, "recomp d3d presenter: readable depth unavailable hr=0x%08lX\n",
            static_cast<unsigned long>(result));
    }
    if (FAILED(result)) {
        return result;
    }
    return result;
}

struct FsrConstants {
    uint32_t con[4][4];
    int32_t origin[2];  // output pixel of the pass's first texel
    int32_t limit[2];   // RCAS source size minus one, for edge clamping
};

/* FSR 1.0 needs Gather (feature level 11_0). EASU and RCAS share one source:
   the input callbacks the FFX headers declare are defined between them. */
void createFsr(RecompD3dPresenter *presenter)
{
#ifdef RECOMP_FSR1
    if (presenter->device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        std::fprintf(stderr, "recomp d3d presenter: fsr needs feature level 11_0; using bilinear\n");
        return;
    }
    std::string source = "#define A_GPU 1\n#define A_HLSL 1\n";
    source.append(kFfxASource, sizeof kFfxASource);
    source +=
        "\nTexture2D<float4> pixels : register(t0);\n"
        "SamplerState bilinear : register(s0);\n"
        "cbuffer Fsr : register(b0) { uint4 con0, con1, con2, con3; int2 origin, limit; };\n"
        "#define FSR_EASU_F 1\n#define FSR_RCAS_F 1\n"
        "AF4 FsrEasuRF(AF2 p) { return pixels.GatherRed(bilinear, p); }\n"
        "AF4 FsrEasuGF(AF2 p) { return pixels.GatherGreen(bilinear, p); }\n"
        "AF4 FsrEasuBF(AF2 p) { return pixels.GatherBlue(bilinear, p); }\n"
        "AF4 FsrRcasLoadF(ASU2 p) { return pixels.Load(int3(clamp(p, int2(0, 0), limit), 0)); }\n"
        "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n";
    source.append(kFfxFsr1Source, sizeof kFfxFsr1Source);
    source +=
        "\nfloat4 vs(uint id : SV_VertexID) : SV_Position {\n"
        " float2 uv = float2((id << 1) & 2, id & 2);\n"
        " return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
        "float4 easu(float4 p : SV_Position) : SV_Target {\n"
        " AF3 c; FsrEasuF(c, AU2(int2(p.xy) - origin), con0, con1, con2, con3); return float4(c, 1); }\n"
        "float4 rcas(float4 p : SV_Position) : SV_Target {\n"
        " AF1 r, g, b; FsrRcasF(r, g, b, AU2(int2(p.xy) - origin), con0); return float4(r, g, b, 1); }\n";
    auto *device = presenter->device;
    ID3DBlob *code[3]{}, *errors = nullptr;
    const char *entries[3] = {"vs", "easu", "rcas"};
    const char *profiles[3] = {"vs_5_0", "ps_5_0", "ps_5_0"};
    HRESULT result = S_OK;
    for (int i = 0; i < 3 && SUCCEEDED(result); ++i) {
        result = D3DCompile(source.data(), source.size(), "fsr1.hlsl", nullptr, nullptr,
            entries[i], profiles[i], D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u, &code[i], &errors);
        if (FAILED(result) && errors != nullptr) {
            std::fprintf(stderr, "recomp d3d presenter: fsr %s: %.*s\n", entries[i],
                static_cast<int>(errors->GetBufferSize()),
                static_cast<const char *>(errors->GetBufferPointer()));
        }
        releaseCom(errors);
    }
    if (SUCCEEDED(result)) result = device->CreateVertexShader(
        code[0]->GetBufferPointer(), code[0]->GetBufferSize(), nullptr, &presenter->fsr_vs);
    if (SUCCEEDED(result)) result = device->CreatePixelShader(
        code[1]->GetBufferPointer(), code[1]->GetBufferSize(), nullptr, &presenter->fsr_easu);
    if (SUCCEEDED(result)) result = device->CreatePixelShader(
        code[2]->GetBufferPointer(), code[2]->GetBufferSize(), nullptr, &presenter->fsr_rcas);
    for (auto *blob : code) releaseCom(blob);
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(FsrConstants);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(result)) result = device->CreateBuffer(&buffer, nullptr, &presenter->fsr_constants);
    if (FAILED(result)) {
        std::fprintf(stderr, "recomp d3d presenter: fsr unavailable hr=0x%08lX; using bilinear\n",
            static_cast<unsigned long>(result));
        releaseCom(presenter->fsr_vs);
        releaseCom(presenter->fsr_easu);
        releaseCom(presenter->fsr_rcas);
        releaseCom(presenter->fsr_constants);
        return;
    }
    std::fprintf(stderr, "recomp d3d presenter: fsr 1.0 on, sharpness %.2f\n", presenter->fsr_sharpness);
#else
    (void)presenter;
    std::fprintf(stderr, "recomp d3d presenter: fsr not built into this runner; using bilinear\n");
#endif
}

/* After a window resize: the back buffer view is the only swap chain
   reference held between frames. A minimized window keeps its old buffers. */
bool resizeSwapChain(RecompD3dPresenter *presenter)
{
    RECT client{};
    if (!GetClientRect(presenter->window, &client) ||
        client.right - client.left <= 0 || client.bottom - client.top <= 0) {
        return true;
    }
    presenter->resize_pending = false;
    const UINT width = static_cast<UINT>(client.right - client.left);
    const UINT height = static_cast<UINT>(client.bottom - client.top);
    DXGI_SWAP_CHAIN_DESC current{};
    presenter->swap_chain->GetDesc(&current);
    if (current.BufferDesc.Width == width && current.BufferDesc.Height == height) return true;
    presenter->context->OMSetRenderTargets(0u, nullptr, nullptr);
    presenter->context->ClearState();
    releaseCom(presenter->present_target_view);
    presenter->context->Flush();
    HRESULT result = presenter->swap_chain->ResizeBuffers(
        0u, width, height, DXGI_FORMAT_UNKNOWN, presenter->swap_flags);
    ID3D11Texture2D *back_buffer = nullptr;
    if (SUCCEEDED(result)) result = presenter->swap_chain->GetBuffer(0u, IID_PPV_ARGS(&back_buffer));
    if (SUCCEEDED(result)) result = presenter->device->CreateRenderTargetView(
        back_buffer, nullptr, &presenter->present_target_view);
    releaseCom(back_buffer);
    if (FAILED(result)) {
        std::fprintf(stderr, "recomp d3d presenter: resize to %ux%u failed hr=0x%08lX\n",
            width, height, static_cast<unsigned long>(result));
        return false;
    }
    return true;
}

/* Draws the main-sized output into the window's back buffer at the largest
   rectangle with the game's aspect, black outside it. FSR runs EASU into a
   letterbox-sized texture, then RCAS into the window; it only upscales. */
void scaleOutput(RecompD3dPresenter *presenter)
{
    auto *context = presenter->context;
    ID3D11Texture2D *back_buffer = nullptr;
    presenter->swap_chain->GetBuffer(0u, IID_PPV_ARGS(&back_buffer));
    if (back_buffer == nullptr) return;
    D3D11_TEXTURE2D_DESC window{};
    back_buffer->GetDesc(&window);
    releaseCom(back_buffer);
    const double aspect = presenter->widescreen ? 16.0 / 9.0 : 4.0 / 3.0;
    uint32_t width = window.Width, height = window.Height;
    if (width > height * aspect) width = static_cast<uint32_t>(height * aspect + 0.5);
    else height = static_cast<uint32_t>(width / aspect + 0.5);
    width = (std::max)(1u, width);
    height = (std::max)(1u, height);
    const int32_t left = static_cast<int32_t>((window.Width - width) / 2u);
    const int32_t top = static_cast<int32_t>((window.Height - height) / 2u);
    const D3D11_VIEWPORT viewport = {static_cast<float>(left), static_cast<float>(top),
        static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    context->ClearState();
    if (width != window.Width || height != window.Height) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context->ClearRenderTargetView(presenter->present_target_view, black);
    }
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->PSSetSamplers(0u, 1u, &presenter->vrr_sampler);
#ifdef RECOMP_FSR1
    // The source is the full-size output, whichever main target set is bound.
    uint32_t source_width = 0u, source_height = 0u;
    {
        ID3D11Resource *resource = nullptr;
        presenter->vrr_source->GetResource(&resource);
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D *>(resource)->GetDesc(&desc);
        releaseCom(resource);
        source_width = desc.Width;
        source_height = desc.Height;
    }
    if (presenter->fsr_easu != nullptr && width >= source_width && height >= source_height &&
        (width > source_width || height > source_height)) {
        if (presenter->fsr_width != width || presenter->fsr_height != height) {
            releaseCom(presenter->fsr_target);
            releaseCom(presenter->fsr_source);
            presenter->fsr_width = presenter->fsr_height = 0u;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = 1u;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1u;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ID3D11Texture2D *texture = nullptr;
            if (SUCCEEDED(presenter->device->CreateTexture2D(&desc, nullptr, &texture)) &&
                SUCCEEDED(presenter->device->CreateRenderTargetView(texture, nullptr, &presenter->fsr_target)) &&
                SUCCEEDED(presenter->device->CreateShaderResourceView(texture, nullptr, &presenter->fsr_source))) {
                presenter->fsr_width = width;
                presenter->fsr_height = height;
            }
            releaseCom(texture);
        }
        if (presenter->fsr_width != 0u) {
            FsrConstants constants{};
            FsrEasuCon(constants.con[0], constants.con[1], constants.con[2], constants.con[3],
                static_cast<AF1>(source_width), static_cast<AF1>(source_height),
                static_cast<AF1>(source_width), static_cast<AF1>(source_height),
                static_cast<AF1>(width), static_cast<AF1>(height));
            context->UpdateSubresource(presenter->fsr_constants, 0u, nullptr, &constants, 0u, 0u);
            const D3D11_VIEWPORT easu_viewport = {0.0f, 0.0f,
                static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            context->RSSetViewports(1u, &easu_viewport);
            context->OMSetRenderTargets(1u, &presenter->fsr_target, nullptr);
            context->VSSetShader(presenter->fsr_vs, nullptr, 0u);
            context->PSSetShader(presenter->fsr_easu, nullptr, 0u);
            context->PSSetConstantBuffers(0u, 1u, &presenter->fsr_constants);
            context->PSSetShaderResources(0u, 1u, &presenter->vrr_source);
            context->Draw(3u, 0u);
            // UI sharpness 1 is RCAS's maximum (0 stops); 0 is two stops softer.
            FsrRcasCon(constants.con[0], 2.0f * (1.0f - presenter->fsr_sharpness));
            constants.origin[0] = left;
            constants.origin[1] = top;
            constants.limit[0] = static_cast<int32_t>(width) - 1;
            constants.limit[1] = static_cast<int32_t>(height) - 1;
            context->UpdateSubresource(presenter->fsr_constants, 0u, nullptr, &constants, 0u, 0u);
            ID3D11ShaderResourceView *none = nullptr;
            context->PSSetShaderResources(0u, 1u, &none);
            context->RSSetViewports(1u, &viewport);
            context->OMSetRenderTargets(1u, &presenter->present_target_view, nullptr);
            context->PSSetShader(presenter->fsr_rcas, nullptr, 0u);
            context->PSSetShaderResources(0u, 1u, &presenter->fsr_source);
            context->Draw(3u, 0u);
            context->ClearState();
            return;
        }
    }
#endif
    context->RSSetViewports(1u, &viewport);
    context->OMSetRenderTargets(1u, &presenter->present_target_view, nullptr);
    context->VSSetShader(presenter->vrr_vs, nullptr, 0u);
    context->PSSetShader(presenter->vrr_ps, nullptr, 0u);
    context->PSSetShaderResources(0u, 1u, &presenter->vrr_source);
    context->Draw(3u, 0u);
    context->ClearState();
}

struct MotionConstants {
    float inverse_camera[16];   // this frame's unjittered clip space to world
    float previous_camera[16];  // world to the previous frame's clip space
    float jitter[2];            // clip-space jitter baked into this frame's depth
    float size[2];              // render target pixels
};

// Row-vector product a * b, as the draw transforms compose.
void multiply4x4(const float a[16], const float b[16], float result[16])
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + c];
            result[r * 4 + c] = sum;
        }
}

// Gauss-Jordan with partial pivoting; false for a singular or non-finite matrix.
bool invert4x4(const float source[16], float result[16])
{
    double m[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) m[r][c] = c < 4 ? source[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(m[r][c]) > std::fabs(m[pivot][c])) pivot = r;
        if (!(std::fabs(m[pivot][c]) > 1e-12)) return false;
        if (pivot != c) for (int k = 0; k < 8; ++k) std::swap(m[c][k], m[pivot][k]);
        const double scale = 1.0 / m[c][c];
        for (int k = 0; k < 8; ++k) m[c][k] *= scale;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double factor = m[r][c];
            for (int k = 0; k < 8; ++k) m[r][k] -= factor * m[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            result[r * 4 + c] = static_cast<float>(m[r][c + 4]);
            if (!std::isfinite(result[r * 4 + c])) return false;
        }
    return true;
}

// Halton low-discrepancy sequence, the jitter pattern temporal upscalers expect.
float halton(uint32_t index, uint32_t base)
{
    float fraction = 1.0f, result = 0.0f;
    while (index > 0u) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

void setFrameJitter(RecompD3dPresenter *presenter)
{
    if (!presenter->jitter_enabled) {
        presenter->jitter[0] = presenter->jitter[1] = 0.0f;
        presenter->jitter_pixels[0] = presenter->jitter_pixels[1] = 0.0f;
        return;
    }
    // RECOMP_D3D_JITTER_SCALE exaggerates the offset so a capture can show what moves.
    static const float exaggerate = [] {
        const char *value = std::getenv("RECOMP_D3D_JITTER_SCALE");
        const float scale = value ? static_cast<float>(std::atof(value)) : 1.0f;
        return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
    }();
    const uint32_t phase = presenter->jitter_index % presenter->jitter_phases + 1u;
    presenter->jitter_pixels[0] = (halton(phase, 2u) - 0.5f) * exaggerate;
    presenter->jitter_pixels[1] = (halton(phase, 3u) - 0.5f) * exaggerate;
    presenter->jitter[0] = presenter->jitter_pixels[0] * 2.0f / static_cast<float>(sceneWidth(presenter));
    presenter->jitter[1] = -presenter->jitter_pixels[1] * 2.0f / static_cast<float>(sceneHeight(presenter));
}

/* Motion vectors in render pixels, current to previous position (the DLSS
   convention). Depth reprojection covers the camera's motion; objects moving
   on their own read as still until per-object vectors exist. */
HRESULT createMotion(RecompD3dPresenter *presenter)
{
    if (!presenter->motion_requested) return S_OK;
    if (presenter->depth_sample == nullptr) {
        std::fprintf(stderr, "recomp motion: readable depth unavailable; disabled\n");
        presenter->motion_requested = presenter->motion_debug = presenter->jitter_enabled = false;
        return S_OK;
    }
    const bool multisampled = presenter->msaa > 1u;
    std::string source = multisampled
        ? "Texture2DMS<float> scene_depth : register(t0);\n"
          "float loadDepth(int2 p) { return scene_depth.Load(p, 0); }\n"
        : "Texture2D<float> scene_depth : register(t0);\n"
          "float loadDepth(int2 p) { return scene_depth.Load(int3(p, 0)); }\n";
    source +=
        "Texture2D<float2> motion : register(t1);\n"
        "Texture2D<float4> object_motion : register(t2);\n"
        "cbuffer Motion : register(b0) {\n"
        "    row_major float4x4 inverse_camera;\n"
        "    row_major float4x4 previous_camera;\n"
        "    float2 jitter;\n"
        "    float2 size;\n"
        "};\n"
        "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
        "    float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
        "float2 reproject(float4 p : SV_Position) : SV_Target {\n"
        "    float depth = loadDepth(int2(p.xy));\n"
        // An object's own vector, if it is still the surface the depth buffer kept.
        "    float4 object = object_motion.Load(int3(p.xy, 0));\n"
        "    if (object.a > 0.5 && abs(object.b - depth) < 2e-5) return object.rg;\n"
        "    float2 ndc = p.xy / size * float2(2, -2) + float2(-1, 1) - jitter;\n"
        "    float4 world = mul(float4(ndc, depth, 1), inverse_camera);\n"
        "    float4 previous = mul(world / world.w, previous_camera);\n"
        "    if (!(previous.w > 0)) return float2(0, 0);\n"
        "    float2 delta = (previous.xy / previous.w - ndc) * float2(0.5, -0.5) * size;\n"
        "    return all(isfinite(delta)) ? delta : float2(0, 0); }\n"
        // Hue is the direction, brightness the length; 16 pixels saturates.
        // size is the output's here; the vectors may be at the smaller DLSS input size.
        "float4 debug(float4 p : SV_Position) : SV_Target {\n"
        "    uint w, h; motion.GetDimensions(w, h);\n"
        "    float2 v = motion.Load(int3(p.xy * float2(w, h) / size, 0));\n"
        "    float length = saturate(sqrt(dot(v, v)) / 16);\n"
        "    float hue = atan2(v.y, v.x) / 6.2831853 + 0.5;\n"
        "    float3 rgb = saturate(abs(frac(hue + float3(0, 2, 1) / 3) * 6 - 3) - 1);\n"
        "    return float4(lerp(float3(0.2, 0.2, 0.2), rgb, length), 0.8); }\n";
    auto *device = presenter->device;
    ID3DBlob *code[3]{}, *errors = nullptr;
    const char *entries[3] = {"vs", "reproject", "debug"};
    const char *profiles[3] = {"vs_4_1", "ps_4_1", "ps_4_1"};
    HRESULT result = S_OK;
    for (int i = 0; i < 3 && SUCCEEDED(result); ++i) {
        result = D3DCompile(source.data(), source.size(), "motion.hlsl", nullptr, nullptr,
            entries[i], profiles[i], 0u, 0u, &code[i], &errors);
        if (FAILED(result) && errors != nullptr) {
            std::fprintf(stderr, "recomp motion: %s: %.*s\n", entries[i],
                static_cast<int>(errors->GetBufferSize()),
                static_cast<const char *>(errors->GetBufferPointer()));
        }
        releaseCom(errors);
    }
    if (SUCCEEDED(result)) result = device->CreateVertexShader(
        code[0]->GetBufferPointer(), code[0]->GetBufferSize(), nullptr, &presenter->motion_vs);
    if (SUCCEEDED(result)) result = device->CreatePixelShader(
        code[1]->GetBufferPointer(), code[1]->GetBufferSize(), nullptr, &presenter->motion_ps);
    if (SUCCEEDED(result)) result = device->CreatePixelShader(
        code[2]->GetBufferPointer(), code[2]->GetBufferSize(), nullptr, &presenter->motion_debug_ps);
    for (auto *blob : code) releaseCom(blob);
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(MotionConstants);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(result)) result = device->CreateBuffer(&buffer, nullptr, &presenter->motion_constants);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = sceneWidth(presenter);
    desc.Height = sceneHeight(presenter);
    desc.MipLevels = desc.ArraySize = 1u;
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    desc.SampleDesc.Count = 1u;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *texture = nullptr;
    if (SUCCEEDED(result)) result = device->CreateTexture2D(&desc, nullptr, &texture);
    if (SUCCEEDED(result)) result = device->CreateRenderTargetView(texture, nullptr, &presenter->motion_target);
    if (SUCCEEDED(result)) result = device->CreateShaderResourceView(texture, nullptr, &presenter->motion_view);
    releaseCom(texture);
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (SUCCEEDED(result)) result = device->CreateBlendState(&blend, &presenter->motion_debug_blend);
    if (FAILED(result)) {
        std::fprintf(stderr, "recomp motion: unavailable hr=0x%08lX\n", static_cast<unsigned long>(result));
        presenter->motion_requested = presenter->motion_debug = presenter->jitter_enabled = false;
        return S_OK;
    }
    // Object vectors share SV_Target1 with the G-buffer and need single-sampled targets.
    if (!presenter->gbuffer.enabled && presenter->msaa == 1u) {
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ID3D11Texture2D *objects = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &objects)) &&
            SUCCEEDED(device->CreateRenderTargetView(objects, nullptr, &presenter->object_motion_target)) &&
            SUCCEEDED(device->CreateShaderResourceView(objects, nullptr, &presenter->object_motion_view))) {
            const float zero[4] = {};
            presenter->context->ClearRenderTargetView(presenter->object_motion_target, zero);
            object_motion_shaders = true;
        } else {
            releaseCom(presenter->object_motion_target);
            releaseCom(presenter->object_motion_view);
        }
        releaseCom(objects);
    }
    setFrameJitter(presenter);
    std::fprintf(stderr, "recomp motion: camera%s vectors on%s%s\n",
        presenter->object_motion_view != nullptr ? " and object" : "",
        presenter->jitter_enabled ? ", jitter" : "", presenter->motion_debug ? ", debug view" : "");
    return S_OK;
}

/* Once per frame, after the 3D scene and before its screen-space UI. A frame
   without a camera, or the first one, reports no motion. */
void runMotionPass(RecompD3dPresenter *presenter)
{
    if (!presenter->motion_requested || presenter->motion_done || presenter->motion_target == nullptr) return;
    presenter->motion_done = true;
    auto *context = presenter->context;
    context->ClearState();
    const float zero[4] = {};
    MotionConstants constants{};
    const float *previous = presenter->has_previous_camera ? presenter->previous_camera : presenter->camera;
    if (!presenter->scene_pending || presenter->camera_weight == 0u ||
        !invert4x4(presenter->camera, constants.inverse_camera)) {
        context->ClearRenderTargetView(presenter->motion_target, zero);
        return;
    }
    std::memcpy(constants.previous_camera, previous, sizeof constants.previous_camera);
    constants.jitter[0] = presenter->jitter[0];
    constants.jitter[1] = presenter->jitter[1];
    constants.size[0] = static_cast<float>(sceneWidth(presenter));
    constants.size[1] = static_cast<float>(sceneHeight(presenter));
    context->UpdateSubresource(presenter->motion_constants, 0u, nullptr, &constants, 0u, 0u);
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, constants.size[0], constants.size[1], 0.0f, 1.0f};
    context->RSSetViewports(1u, &viewport);
    context->OMSetRenderTargets(1u, &presenter->motion_target, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(presenter->motion_vs, nullptr, 0u);
    context->PSSetShader(presenter->motion_ps, nullptr, 0u);
    context->PSSetConstantBuffers(0u, 1u, &presenter->motion_constants);
    ID3D11ShaderResourceView *views[3] = {presenter->depth_sample, nullptr, presenter->object_motion_view};
    context->PSSetShaderResources(0u, 3u, views);
    context->Draw(3u, 0u);
    context->ClearState();
}

// RECOMP_D3D_MOTION_DEBUG: blends the vectors over the frame's main-sized output.
void drawMotionDebug(RecompD3dPresenter *presenter, ID3D11RenderTargetView *output)
{
    if (!presenter->motion_debug || presenter->motion_view == nullptr) return;
    auto *context = presenter->context;
    context->ClearState();
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(mainWidth(presenter)),
        static_cast<float>(mainHeight(presenter)), 0.0f, 1.0f};
    MotionConstants constants{};
    constants.size[0] = viewport.Width;
    constants.size[1] = viewport.Height;
    context->UpdateSubresource(presenter->motion_constants, 0u, nullptr, &constants, 0u, 0u);
    context->PSSetConstantBuffers(0u, 1u, &presenter->motion_constants);
    context->RSSetViewports(1u, &viewport);
    context->OMSetRenderTargets(1u, &output, nullptr);
    context->OMSetBlendState(presenter->motion_debug_blend, nullptr, 0xffffffffu);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(presenter->motion_vs, nullptr, 0u);
    context->PSSetShader(presenter->motion_debug_ps, nullptr, 0u);
    ID3D11ShaderResourceView *views[2] = {nullptr, presenter->motion_view};
    context->PSSetShaderResources(0u, 2u, views);
    context->Draw(3u, 0u);
    context->ClearState();
}

double steadyMs();

// Exchanges the main target set in use with `other` (scene input <-> full size).
void swapMainTargets(RecompD3dPresenter *presenter)
{
    auto &other = presenter->other;
    std::swap(presenter->render_target_view, other.color);
    std::swap(presenter->depth_texture, other.depth_texture);
    std::swap(presenter->depth_view, other.depth_view);
    std::swap(presenter->depth_sample, other.depth_sample);
    std::swap(presenter->back_buffer_copy, other.back_copy);
    std::swap(presenter->back_buffer_sample, other.back_sample);
    presenter->scene_phase = !presenter->scene_phase;
}

void releaseDlss(RecompD3dPresenter *presenter)
{
    if (presenter->scene_phase) swapMainTargets(presenter);
    auto &other = presenter->other;
    releaseCom(other.color);
    releaseCom(other.depth_texture);
    releaseCom(other.depth_view);
    releaseCom(other.depth_sample);
    releaseCom(other.back_copy);
    releaseCom(other.back_sample);
    releaseCom(presenter->scene_color);
    releaseCom(presenter->dlss_output);
    releaseCom(presenter->dlss_output_view);
    releaseCom(presenter->dlss_copy_vs);
    releaseCom(presenter->dlss_copy_ps);
    releaseCom(presenter->dlss_sampler);
    if (presenter->dlss_shutdown != nullptr) presenter->dlss_shutdown();
    presenter->dlss_evaluate = nullptr;
    presenter->dlss_shutdown = nullptr;
    presenter->dlss_active = false;
}

/* Loads dlss_bridge.dll beside the runner and builds the scene input set at
   DLSS's optimal size for the chosen mode. Any failure leaves DLSS off and the
   frame renders as before. Called after the full-size main set exists. */
void createDlss(RecompD3dPresenter *presenter)
{
    if (presenter->dlss_mode < 0) return;
    const auto off = [presenter](const char *why) {
        std::fprintf(stderr, "recomp dlss: %s; off\n", why);
        releaseDlss(presenter);
        presenter->dlss_mode = -1;
    };
    if (presenter->dlss_module == nullptr) presenter->dlss_module = LoadLibraryW(L"dlss_bridge.dll");
    if (presenter->dlss_module == nullptr) return off("dlss_bridge.dll not found beside the runner");
    const auto symbol = [presenter](const char *name) {
        return reinterpret_cast<void *>(GetProcAddress(presenter->dlss_module, name));
    };
    const auto init = reinterpret_cast<DlssBridgeInit>(symbol("dlss_bridge_init"));
    const auto render_size = reinterpret_cast<DlssBridgeRenderSize>(symbol("dlss_bridge_render_size"));
    const auto create = reinterpret_cast<DlssBridgeCreate>(symbol("dlss_bridge_create"));
    presenter->dlss_evaluate = reinterpret_cast<DlssBridgeEvaluate>(symbol("dlss_bridge_evaluate"));
    presenter->dlss_error = reinterpret_cast<DlssBridgeError>(symbol("dlss_bridge_error"));
    const auto shutdown = reinterpret_cast<DlssBridgeShutdown>(symbol("dlss_bridge_shutdown"));
    if (!init || !render_size || !create || !presenter->dlss_evaluate || !presenter->dlss_error || !shutdown)
        return off("dlss_bridge.dll is incomplete");
    wchar_t directory[MAX_PATH] = L".";
    const DWORD length = GetModuleFileNameW(nullptr, directory, MAX_PATH);
    if (length != 0u && length < MAX_PATH) {
        wchar_t *slash = std::wcsrchr(directory, L'\\');
        if (slash != nullptr) *slash = L'\0';
    }
    if (!init(presenter->device, directory)) return off(presenter->dlss_error());
    presenter->dlss_shutdown = shutdown;

    const uint32_t output_width = mainWidth(presenter), output_height = mainHeight(presenter);
    unsigned render_width = output_width, render_height = output_height;
    if (presenter->dlss_mode != DLSS_BRIDGE_DLAA &&
        !render_size(output_width, output_height, presenter->dlss_mode, &render_width, &render_height))
        return off(presenter->dlss_error());

    auto *device = presenter->device;
    auto &other = presenter->other;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = render_width;
    desc.Height = render_height;
    desc.MipLevels = desc.ArraySize = 1u;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1u;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *texture = nullptr;
    HRESULT result = device->CreateTexture2D(&desc, nullptr, &texture);
    if (SUCCEEDED(result)) result = device->CreateRenderTargetView(texture, nullptr, &other.color);
    if (SUCCEEDED(result)) result = device->CreateShaderResourceView(texture, nullptr, &presenter->scene_color);
    releaseCom(texture);
    desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(result)) result = device->CreateTexture2D(&desc, nullptr, &other.depth_texture);
    D3D11_DEPTH_STENCIL_VIEW_DESC depth_desc{};
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (SUCCEEDED(result)) result = device->CreateDepthStencilView(other.depth_texture, &depth_desc, &other.depth_view);
    D3D11_SHADER_RESOURCE_VIEW_DESC sample_desc{};
    sample_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sample_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sample_desc.Texture2D.MipLevels = 1u;
    if (SUCCEEDED(result)) result = device->CreateShaderResourceView(other.depth_texture, &sample_desc, &other.depth_sample);
    desc.Width = output_width;
    desc.Height = output_height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(result)) result = device->CreateTexture2D(&desc, nullptr, &presenter->dlss_output);
    if (SUCCEEDED(result)) result = device->CreateShaderResourceView(presenter->dlss_output, nullptr, &presenter->dlss_output_view);
    static const char shader[] =
        "Texture2D pixels : register(t0);\n"
        "SamplerState bilinear : register(s0);\n"
        "float4 vs(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position {\n"
        " uv = float2((id << 1) & 2, id & 2);\n"
        " return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
        "float4 ps(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        " return float4(pixels.SampleLevel(bilinear, uv, 0).rgb, 1); }\n";
    ID3DBlob *vertex = nullptr, *pixel = nullptr;
    if (SUCCEEDED(result)) result = D3DCompile(shader, sizeof shader - 1u, nullptr, nullptr, nullptr,
        "vs", "vs_4_0", 0u, 0u, &vertex, nullptr);
    if (SUCCEEDED(result)) result = D3DCompile(shader, sizeof shader - 1u, nullptr, nullptr, nullptr,
        "ps", "ps_4_0", 0u, 0u, &pixel, nullptr);
    if (SUCCEEDED(result)) result = device->CreateVertexShader(
        vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &presenter->dlss_copy_vs);
    if (SUCCEEDED(result)) result = device->CreatePixelShader(
        pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &presenter->dlss_copy_ps);
    releaseCom(vertex);
    releaseCom(pixel);
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(result)) result = device->CreateSamplerState(&sampler, &presenter->dlss_sampler);
    if (FAILED(result)) return off("could not create its targets");
    if (!create(presenter->context, render_width, render_height, output_width, output_height, presenter->dlss_mode))
        return off(presenter->dlss_error());

    presenter->dlss_render_width = render_width;
    presenter->dlss_render_height = render_height;
    presenter->dlss_active = true;
    presenter->dlss_reset = true;
    // NVIDIA's guide: at least 8 phases, times the square of the upscale ratio.
    const double ratio = static_cast<double>(output_height) / render_height;
    presenter->jitter_phases = (std::max)(8u, static_cast<uint32_t>(std::ceil(8.0 * ratio * ratio)));
    swapMainTargets(presenter);  // frames start in the scene phase
    static const char *names[] = {"DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"};
    std::fprintf(stderr, "recomp dlss: %s, scene %ux%u -> %ux%u, %u jitter phases\n",
        names[presenter->dlss_mode], render_width, render_height, output_width, output_height,
        presenter->jitter_phases);
}

/* Ends the frame's 3D scene: motion vectors, then DLSS reconstructs the scene
   into the full-size set, which the HUD and the output pass use. A failed
   evaluation scales the scene bilinearly instead, so the frame still shows. */
void finishScene(RecompD3dPresenter *presenter)
{
    runMotionPass(presenter);
    if (!presenter->dlss_active || !presenter->scene_phase) return;
    auto *context = presenter->context;
    context->ClearState();
    ID3D11Resource *color = nullptr, *motion = nullptr;
    presenter->render_target_view->GetResource(&color);
    presenter->motion_target->GetResource(&motion);
    const double now = steadyMs();
    DlssBridgeFrame frame{};
    frame.color = color;
    frame.depth = presenter->depth_texture;
    frame.motion = motion;
    frame.output = presenter->dlss_output;
    frame.jitter_x = presenter->jitter_pixels[0];
    frame.jitter_y = presenter->jitter_pixels[1];
    frame.render_width = presenter->dlss_render_width;
    frame.render_height = presenter->dlss_render_height;
    frame.reset = presenter->dlss_reset || !presenter->has_previous_camera ? 1 : 0;
    frame.frame_ms = presenter->dlss_last_ms != 0.0 ? static_cast<float>(now - presenter->dlss_last_ms) : 16.7f;
    presenter->dlss_last_ms = now;
    const bool upscaled = presenter->dlss_evaluate(context, &frame) != 0;
    releaseCom(color);
    releaseCom(motion);
    static bool reported = false;
    if (!upscaled && !reported) {
        reported = true;
        std::fprintf(stderr, "recomp dlss: %s; scaling the scene bilinearly\n", presenter->dlss_error());
    }
    presenter->dlss_reset = false;
    context->ClearState();
    swapMainTargets(presenter);
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(mainWidth(presenter)),
        static_cast<float>(mainHeight(presenter)), 0.0f, 1.0f};
    context->RSSetViewports(1u, &viewport);
    context->OMSetRenderTargets(1u, &presenter->render_target_view, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(presenter->dlss_copy_vs, nullptr, 0u);
    context->PSSetShader(presenter->dlss_copy_ps, nullptr, 0u);
    context->PSSetSamplers(0u, 1u, &presenter->dlss_sampler);
    ID3D11ShaderResourceView *source = upscaled ? presenter->dlss_output_view : presenter->scene_color;
    context->PSSetShaderResources(0u, 1u, &source);
    context->Draw(3u, 0u);
    // The HUD draws over the reconstructed scene with no scene depth left.
    context->ClearDepthStencilView(presenter->depth_view, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0u);
    context->ClearState();
}

// After a present: this frame's camera becomes the previous one and the jitter advances.
void endMotionFrame(RecompD3dPresenter *presenter)
{
    if (presenter->dlss_active && !presenter->scene_phase) swapMainTargets(presenter);
    if (!presenter->motion_requested) return;
    presenter->has_previous_camera = presenter->camera_weight != 0u;
    if (presenter->has_previous_camera) {
        std::memcpy(presenter->previous_camera, presenter->camera, sizeof presenter->camera);
    }
    presenter->camera_weight = 0u;
    presenter->scene_pending = presenter->motion_done = false;
    if (presenter->object_motion_target != nullptr) {
        std::swap(presenter->objects_previous, presenter->objects_current);
        presenter->objects_current.clear();
        presenter->objects_recorded = 0u;
        const float zero[4] = {};
        presenter->context->ClearRenderTargetView(presenter->object_motion_target, zero);
    }
    ++presenter->jitter_index;
    setFrameJitter(presenter);
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
    if (SUCCEEDED(result) && outputScaled(presenter)) {
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
        // FSR is optional: a failure here leaves the bilinear scale.
        if (SUCCEEDED(result) && presenter->fsr_requested) createFsr(presenter);
    }
    releaseCom(back_buffer);
    if (FAILED(result)) {
        return result;
    }

    result = createMainDepth(presenter);
    if (FAILED(result)) return result;
    createDlss(presenter);    // switches to the scene set, which motion vectors measure
    createMotion(presenter);
    // DLSS reads the motion target every frame; without it, render as before.
    if (presenter->dlss_active && (!presenter->motion_requested || presenter->motion_target == nullptr)) {
        std::fprintf(stderr, "recomp dlss: motion vectors unavailable; off\n");
        releaseDlss(presenter);
        presenter->dlss_mode = -1;
    }
    if (presenter->gbuffer.enabled) {
        if (presenter->depth_sample) {
            presenter->gbuffer.create(presenter->device, mainWidth(presenter), mainHeight(presenter), presenter->msaa);
            if (presenter->gbuffer.target) {
                const float zero[4]{};
                presenter->context->ClearRenderTargetView(presenter->gbuffer.target, zero);
            }
        } else {
            std::fprintf(stderr, "recomp gbuffer: readable depth unavailable; disabled\n");
            presenter->gbuffer.release();
        }
    }
#ifdef RECOMP_RTGI
    if (presenter->rtgi_requested && presenter->depth_sample != nullptr) {
        presenter->rtgi = d3d_rtgi_create(
            presenter->device, mainWidth(presenter), mainHeight(presenter),
            presenter->performance_counter);
        if (presenter->rtgi != nullptr) {
            std::fprintf(stderr, "recomp rtgi: enabled at %ux%u\n",
                mainWidth(presenter), mainHeight(presenter));
        }
    }
#endif

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
        (presenter->scale != 1.0f || presenter->msaa > 1u || presenter->dlss_active)) {
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
    if (presenter->gbuffer.enabled && !presenter->sun.casting && !target.offscreen &&
        depth_view == presenter->depth_view) {
        ID3D11RenderTargetView *targets[] = {color_view, presenter->gbuffer.target};
        presenter->context->OMSetRenderTargets(2u, targets, depth_view);
    } else if (presenter->object_motion_target != nullptr && !presenter->sun.casting &&
        !target.offscreen && depth_view == presenter->depth_view &&
        (!presenter->dlss_active || presenter->scene_phase)) {
        ID3D11RenderTargetView *targets[] = {color_view, presenter->object_motion_target};
        presenter->context->OMSetRenderTargets(2u, targets, depth_view);
    } else {
        presenter->context->OMSetRenderTargets(1u, &color_view, depth_view);
    }
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

    if (presenter->gbuffer.enabled && !clear.target.offscreen &&
        (clear.clear_color || (clear.clear_depth && depth_view == presenter->depth_view))) {
        const float zero[4]{};
        presenter->context->ClearRenderTargetView(presenter->gbuffer.target, zero);
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
        if (presenter->sun.enabled && clear.target.offscreen)
            if (auto *entry = findRenderTarget(presenter, clear.target.color))
                entry->sun_palm.clear(clear.color);
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

bool pixelLightingEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("RECOMP_D3D_PIXEL_LIGHTING");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

void replaceAll(std::string &text, const std::string &from, const std::string &to)
{
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
}

/* Object motion vectors (docs/motion-vectors.md). Fixed-function vertex
   shaders also place the vertex with last frame's matrices; the pixel shader
   writes the difference in pixels and its depth to SV_Target1 when
   motion_flags.x is set. Vertex programs keep camera vectors.
   Other variants (sun shadows, per-pixel lighting, vertex programs) edit the
   same source first, so every anchor is checked before anything changes; a
   source without them is left as it was and its draws keep camera vectors.
   Edits run from the end of the source back, so earlier offsets stay valid. */
void objectMotionShaderSource(std::string &source)
{
    constexpr size_t npos = std::string::npos;
    const std::string fields = "struct VSOut {", output = "VSOut output;";
    const std::string lit = "output.lit_specular=0;\n";
    const std::string jitter = "    output.position.xy += taa_jitter.xy * output.position.w;\n";
    const size_t fields_at = source.find(fields);
    const size_t output_at = source.find(output);
    const size_t pixel = source.find("float4 ps_main(");
    const size_t signature_end = pixel == npos ? npos : source.find(") : SV_TARGET", pixel);
    const size_t body = signature_end == npos ? npos : source.find('{', signature_end);
    if (fields_at == npos || output_at == npos || body == npos ||
        fields_at > output_at || output_at > pixel) return;

    source.insert(body + 1, R"(
    motion_output = 0;
    if (motion_flags.x > 0.5f && input.motion_current.w > 0 && input.motion_previous.w > 0) {
        float2 now = input.motion_current.xy / input.motion_current.w;
        float2 before = input.motion_previous.xy / input.motion_previous.w;
        motion_output = float4((before - now) * float2(0.5f, -0.5f) * motion_flags.zw, input.position.z, 1);
    }
)");
    source.insert(signature_end, ", out float4 motion_output : SV_Target1");
    // Fixed function: repeat the position code with last frame's matrices.
    const size_t jitter_at = source.find(jitter, output_at);
    const size_t lit_at = source.find(lit, output_at);
    if (jitter_at != npos && jitter_at < pixel && lit_at != npos && lit_at < jitter_at) {
        const size_t position_at = lit_at + lit.size();
        std::string previous = source.substr(position_at, jitter_at - position_at);
        replaceAll(previous, "wvp[", "previous_wvp[");
        replaceAll(previous, "output.position", "motion_previous");
        source.insert(jitter_at, "    output.motion_current = output.position;\n"
            "    float4 motion_previous;\n" + previous +
            "    output.motion_previous = motion_previous;\n");
    }
    source.insert(output_at + output.size(), " output.motion_current = 0; output.motion_previous = 0;");
    // TEXCOORD9/10: the slots no other variant (pixel lighting, sun, G-buffer) uses.
    source.insert(fields_at + fields.size(),
        "\n    float4 motion_current : TEXCOORD9;\n    float4 motion_previous : TEXCOORD10;\n");
}

/* Builds the HLSL for one FVF, plus its vertex program when it has one. */
bool drawShaderSource(
    uint32_t fvf,
    const DrawPipeline &pipeline,
    RecompD3dVertexLayout &layout,
    std::string &compiled_source, bool explicit_coverage = true, bool sun_shadow = false,
    bool pixel_lighting = pixelLightingEnabled(), bool gbuffer = false)
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
    if (!pipeline.program_count &&
        layout.normal_offset != RECOMP_D3D_FVF_ABSENT && !layout.pretransformed &&
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
        const std::string evaluate =
            "float3 evaluateLighting(float3 n, float3 world_pos, out float3 spec) {\n"
            "        float3 rgb=directional_base.rgb; spec=0;\n"
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
            "            float3 h = l + light_view.xyz;\n"
            "            float h2 = dot(h,h);\n"
            "            if (light_view.w > 0 && dot(n,l) > 0 && h2 > 0)\n"
            "                spec += atten * light_specular[i].rgb * pow(max(dot(n,h*rsqrt(h2)),1e-6f),light_view.w);\n"
            "        }\n"
            "        spec=saturate(spec);\n"
            "        return saturate(rgb);\n"
            "}\n";
        const auto vertex = compiled_source.find("VSOut vs_main(");
        if (vertex == std::string::npos) return false;
        compiled_source.insert(vertex, evaluate);
        // Sun shadows anchor on this line; it exists in both lighting modes.
        lighting += "        if (directional_flags.y > 0.5f && dot(n,n)>0) n *= rsqrt(dot(n,n));\n";
        if (pixel_lighting) {
            // TEXCOORD6..10 belong to fog and sun shadows.
            compiled_source.insert(compiled_source.find("struct VSOut {") + std::strlen("struct VSOut {"),
                "\n    float3 light_position : TEXCOORD11;\n    float4 light_normal : TEXCOORD12;\n");
            // Initialize unlit draws too; their existing color path stays intact.
            lighting.insert(0, "    output.light_position = 0; output.light_normal = 0;\n");
            lighting += "        output.light_position = world_pos;\n"
                "        output.light_normal = float4(n, length(n));\n";
            const std::string pixel = "float4 ps_main(VSOut input) : SV_TARGET {";
            const auto sample = compiled_source.find(pixel);
            if (sample == std::string::npos) return false;
            // Preserve magnitude when NORMALIZENORMALS is off, including flat lighting.
            compiled_source.insert(sample + pixel.size(),
                "\n    if (directional_flags.x > 0.5f) {\n"
                "        float3 n = input.light_normal.xyz;\n"
                "        if (dot(n,n)>0) n *= rsqrt(dot(n,n));\n"
                "        if (directional_flags.y < 0.5f) n *= input.light_normal.w;\n"
                "        input.color.rgb = evaluateLighting(n, input.light_position, input.lit_specular);\n"
                "    }\n");
        } else {
            lighting += "        output.color.rgb = evaluateLighting(n, world_pos, output.lit_specular);\n";
        }
        lighting += "    }\n";
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
        compiled_source.replace(begin, end-begin, "VSOut vs_main(VSIn input) { VSOut output; output.lit_specular=0;\n" + body + "return output; }\n");
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
    if (sun_shadow && !pipeline.program_count) sunShaderSource(compiled_source);
    if (explicit_coverage) {
        const std::string pixel = "float4 ps_main(VSOut input) : SV_TARGET {";
        compiled_source.replace(compiled_source.find(pixel), pixel.size(),
            "float4 ps_main(VSOut input, out uint sample_coverage : SV_Coverage) : SV_TARGET {\n"
            "    sample_coverage = 0xffffffffu;");
        compiled_source.insert(0, "#define EXPLICIT_COVERAGE 1\n");
    }
    if (gbuffer) gbufferShaderSource(compiled_source, layout, pipeline.program_count != 0);
    else if (object_motion_shaders) objectMotionShaderSource(compiled_source);
    return true;
}

/* Every FVF seen across all logged runs (14). Compiling them at boot keeps
   first use of each scene from stalling ~85 ms per shader. */
constexpr uint32_t kBootDrawFvfs[] = {
    0x042u, 0x104u, 0x112u, 0x116u, 0x118u, 0x11Au,
    0x142u, 0x144u, 0x212u, 0x216u, 0x21Au, 0x242u, 0x244u, 0x404u};

// ponytail: fixed list; vertex-program shaders and unlisted FVFs still compile on first use.
void precompileDrawShaders(const std::atomic<bool> *stop, bool explicit_coverage, bool sun_shadow = false, bool gbuffer = false)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    const ULONGLONG start = GetTickCount64();
    static const DrawPipeline no_program{};
    uint32_t compiled = 0u;
    try {
        for (const uint32_t fvf : kBootDrawFvfs) {
            RecompD3dVertexLayout layout;
            std::string source;
            if (!drawShaderSource(fvf, no_program, layout, source, explicit_coverage, sun_shadow, pixelLightingEnabled(), gbuffer)) continue;
            for (const char *stage : {"vs", "ps"}) {
                if (stop->load()) return;
                ID3DBlob *blob = nullptr;
                const bool vertex = stage[0] == 'v';
                compiled += compileDrawShader(source.c_str(), vertex ? "vs_main" : "ps_main",
                    vertex ? "vs_4_0" : explicit_coverage ? "ps_4_1" : "ps_4_0", &blob);
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
    const bool explicit_coverage = presenter->device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_10_1;
    if (!drawShaderSource(fvf, pipeline, layout, compiled_source, explicit_coverage, presenter->sun.enabled,
            pixelLightingEnabled(), presenter->gbuffer.enabled)) {
        return false;
    }

    ID3DBlob *vertex_blob = nullptr;
    ID3DBlob *pixel_blob = nullptr;
    if (!compileDrawShader(compiled_source.c_str(), "vs_main", "vs_4_0", &vertex_blob) ||
        !compileDrawShader(compiled_source.c_str(), "ps_main", explicit_coverage ? "ps_4_1" : "ps_4_0", &pixel_blob)) {
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
       point positions, attenuation/range and material-scaled ambient, specular and viewer (336 floats). */
    constant_desc.ByteWidth = static_cast<UINT>(kDrawConstantFloats * sizeof(float));
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
    UINT &offset,
    UINT skip = 0u,
    UINT copied = UINT_MAX)
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
    // Bytes outside [skip, skip+copied) keep stale ring data that no index reads.
    std::memcpy(static_cast<uint8_t *>(mapped.pData) + used + skip, source, (std::min)(copied, size - skip));
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
    ID3D11ShaderResourceView *none[2] = {};
    presenter->context->PSSetShaderResources(0u, 2u, none);
    copyGuestBuffer(presenter, presenter->back_buffer_copy);
    /* Every view exposes the chain; refresh it with each new snapshot. */
    if (presenter->scale != 1.0f)
        presenter->context->GenerateMips(presenter->back_buffer_sample);
    return presenter->back_buffer_sample;
}

/* Xbox D3DTADDRESS WRAP..BORDER (1..4) share D3D11's values and CLAMPTOEDGE
   clamps. The casino Zack sprite draws V 0..2 with CLAMP; wrap shows it twice. */
ID3D11SamplerState *lookupDrawSampler(
    RecompD3dPresenter *presenter, uint32_t address_u, uint32_t address_v,
    bool anisotropic = false)
{
    const auto host = [](uint32_t mode) {
        return mode == 5u ? D3D11_TEXTURE_ADDRESS_CLAMP : mode >= 1u && mode <= 4u
            ? static_cast<D3D11_TEXTURE_ADDRESS_MODE>(mode) : D3D11_TEXTURE_ADDRESS_WRAP;
    };
    if (presenter->draw_sampler == nullptr) return nullptr;
    // The forced setting upgrades the shared linear sampler; point-filtered draws build their own.
    anisotropic = anisotropic || presenter->anisotropy != 0u;
    const D3D11_TEXTURE_ADDRESS_MODE u = host(address_u), v = host(address_v);
    ID3D11SamplerState *&sampler = presenter->address_samplers[anisotropic][u - 1][v - 1];
    if (sampler == nullptr) {
        D3D11_SAMPLER_DESC desc{};
        presenter->draw_sampler->GetDesc(&desc);
        desc.AddressU = u;
        desc.AddressV = v;
        if (anisotropic) {
            desc.Filter = D3D11_FILTER_ANISOTROPIC;
            desc.MaxAnisotropy = presenter->anisotropy != 0u ? presenter->anisotropy : 16u;
        }
        if (FAILED(presenter->device->CreateSamplerState(&desc, &sampler))) {
            return presenter->draw_sampler;
        }
    }
    return sampler;
}

void unindexTexture(RecompD3dPresenter *presenter, uint32_t slot)
{
    auto &identity = presenter->textures[slot].identity;
    presenter->texture_identity_bytes -= identity.bytes.size();
    identity = {};
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
    const RecompD3dPresenterDrawCommand &draw,
    SunOverlay *sun_overlay = nullptr)
{
    if (sun_overlay) *sun_overlay = SunOverlay::None;
    const RecompD3dTextureDesc &desc = draw.texture;
    const bool palettized = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_P8;
    const bool linear_bgra = desc.format_byte == 0x12u;
    const bool compressed = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT1 ||
        desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT3 || desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT5;
    const uint32_t levels = !linear_bgra && desc.mip_levels ? desc.mip_levels : 1u;
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
        if (sun_overlay) *sun_overlay = sunPalmTexture(desc) && entry->sun_palm.valid() ? SunOverlay::Palm : SunOverlay::None;
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
    const bool replaceable = presenter->replacements.enabled() && !desc.linear &&
        !desc.depth && draw.texture_bytes &&
        draw.texture_byte_count == recomp_d3d_texture_mip_span(&desc);
    const auto cached = presenter->texture_index.equal_range(desc.data);
    const bool fingerprinted = !linear_bgra && draw.texture_bytes != nullptr;
    // The capture-time fingerprint keeps a texture the guest has begun to refill.
    const uint64_t fingerprint = !fingerprinted ? 0u : draw.texture_fingerprint ? draw.texture_fingerprint
        : textureFingerprint(draw.texture_bytes, draw.texture_byte_count);
    for (auto it = cached.first; it != cached.second; ++it) {
        TextureEntry &entry = presenter->textures[it->second];

        if (entry.format_byte == desc.format_byte &&
            entry.width == desc.width && entry.height == desc.height && entry.mip_levels == levels &&
            (!palettized || std::memcmp(entry.palette, draw.palette_bytes, kPaletteBytes) == 0)) {
            if (fingerprinted && (entry.fingerprint != fingerprint ||
                (replaceable && !entry.identity.key.empty() && !entry.identity.matches(draw)))) {
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
            if (sun_overlay) *sun_overlay = entry.sun_overlay;
            return replaceable ? presenter->replacements.lookup(entry.identity.key, entry.view) : entry.view;
        }
    }
    const DXGI_FORMAT format = linear_bgra
        ? DXGI_FORMAT_B8G8R8A8_UNORM : hostTextureFormat(desc.format_byte);
    if (format == DXGI_FORMAT_UNKNOWN || draw.texture_bytes == nullptr ||
        draw.texture_byte_count == 0u || (desc.linear && !linear_bgra)) {
        return nullptr;
    }
    // Upload and fingerprint one copy: the guest may still be streaming into the
    // borrowed bytes, and a torn upload must not match the finished texture.
    std::vector<uint8_t> snapshot;
    const void *texture_bytes = draw.texture_bytes;
    if (!linear_bgra) {
        const auto *source = static_cast<const uint8_t *>(draw.texture_bytes);
        snapshot.assign(source, source + draw.texture_byte_count);
        texture_bytes = snapshot.data();
    }
    SunOverlay overlay = presenter->sun.enabled &&
        desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT5 &&
        draw.texture_byte_count == recomp_d3d_texture_compressed_mip_span(&desc) ?
        sunStaticOverlay(desc, sunPropPayloadHash(texture_bytes, draw.texture_byte_count)) : SunOverlay::None;
    if (presenter->sun.enabled && overlay == SunOverlay::None &&
        sunShadowTexels(desc, static_cast<const uint8_t *>(texture_bytes), draw.texture_byte_count)) {
        overlay = SunOverlay::Palm;
        std::fprintf(stderr, "recomp sun shadow: legacy tree overlay data=0x%08X size=%ux%u mips=%u\n",
            desc.data, desc.width, desc.height, levels);
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
    std::vector<std::vector<uint8_t>> mip_pixels(levels);

    if (linear_bgra) {
        initial.pSysMem = draw.texture_bytes;
        initial.SysMemPitch = desc.pitch;
    } else if (palettized || isSwizzledTextureFormat(desc.format_byte)) {
        const uint32_t texel_bytes = palettized ? 1u : desc.bits_per_pixel / 8u;
        const uint32_t span = recomp_d3d_texture_mip_span(&desc);
        if (span == 0u || span > draw.texture_byte_count ||
            desc.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            desc.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) return nullptr;
        uint32_t width = desc.width, height = desc.height, offset = 0u;
        for (uint32_t level = 0u; level < levels; ++level) {
            const size_t texels = size_t(width) * height;
            auto &pixels = mip_pixels[level];
            pixels.resize(texels * (palettized ? 4u : texel_bytes));
            const auto *source = static_cast<const uint8_t *>(texture_bytes) + offset;
            if (palettized) {
                std::vector<uint8_t> indices(texels);
                if (!recomp_d3d_texture_unswizzle(source, indices.data(), width, height, 1u)) return nullptr;
                const auto *palette = static_cast<const uint8_t *>(draw.palette_bytes);
                for (size_t i = 0u; i < texels; ++i)
                    std::memcpy(pixels.data() + i * 4u, palette + indices[i] * 4u, 4u);
            } else if (!recomp_d3d_texture_unswizzle(source, pixels.data(), width, height, texel_bytes)) {
                return nullptr;
            }
            subresources[level].pSysMem = pixels.data();
            subresources[level].SysMemPitch = width * (palettized ? 4u : texel_bytes);
            offset += static_cast<uint32_t>(texels * texel_bytes);
            width = (std::max)(1u, width / 2u); height = (std::max)(1u, height / 2u);
        }
    } else {
        const uint32_t block_bytes = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT1 ? 8u : 16u;
        const auto *bytes = static_cast<const uint8_t *>(texture_bytes);
        if (overlay == SunOverlay::NetPost) {
            mip_pixels[0].assign(bytes, bytes + draw.texture_byte_count);
            sunKeepNetLine(desc, mip_pixels[0].data(), levels);
            bytes = mip_pixels[0].data();
        }
        uint32_t width = desc.width, height = desc.height, offset = 0u;
        for (uint32_t level = 0; level < levels; ++level) {
            subresources[level].pSysMem = bytes + offset;
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
    // A new entry holds the snapshot, which may be newer than the capture.
    entry.fingerprint = fingerprinted ? textureFingerprint(texture_bytes, draw.texture_byte_count) : fingerprint;
    entry.sun_overlay = overlay;
    if (sun_overlay) *sun_overlay = entry.sun_overlay;
    presenter->texture_identity_bytes -= entry.identity.bytes.size();
    entry.identity = {};
    if (replaceable && presenter->texture_identity_bytes + draw.texture_byte_count +
        kPaletteBytes <= 128u * 1024u * 1024u) {
        try { entry.identity.assign(draw); }
        catch (const std::exception &) { entry.identity = {}; }
    }
    presenter->texture_identity_bytes += entry.identity.bytes.size();
    entry.view = view;
    try {
        presenter->texture_index.emplace(desc.data, slot);
    } catch (const std::bad_alloc &) {
        releaseCom(entry.view);
        entry.used = false;
        return nullptr;
    }
    return replaceable ? presenter->replacements.lookup(entry.identity.key, view) : view;
}

ID3D11BlendState *lookupBlendState(
    RecompD3dPresenter *presenter,
    const RecompD3dBlendState &blend,
    bool alpha_to_coverage = false, bool normal_write = false)
{
    for (uint32_t i = 0u; i < presenter->blend_state_count; ++i) {
        BlendStateEntry &entry = presenter->blend_states[i];

        if (entry.used && entry.enable == blend.blend_enable &&
            entry.alpha_to_coverage == alpha_to_coverage && entry.normal_write == normal_write &&
            entry.src == blend.src_factor && entry.dst == blend.dst_factor &&
            entry.op == blend.op &&
            entry.color_write_mask == blend.color_write_mask) {
            return entry.state;
        }
    }

    D3D11_BLEND_DESC desc{};
    desc.AlphaToCoverageEnable = alpha_to_coverage;
    const bool second_target = presenter->gbuffer.enabled || object_motion_shaders;
    desc.IndependentBlendEnable = second_target;
    D3D11_RENDER_TARGET_BLEND_DESC &target = desc.RenderTarget[0];
    target.BlendEnable = blend.blend_enable ? TRUE : FALSE;
    target.SrcBlend = hostBlendFactor(blend.src_factor, false);
    target.DestBlend = hostBlendFactor(blend.dst_factor, false);
    target.BlendOp = hostBlendOp(blend.op);
    target.SrcBlendAlpha = hostBlendFactor(blend.src_factor, true);
    target.DestBlendAlpha = hostBlendFactor(blend.dst_factor, true);
    target.BlendOpAlpha = hostBlendOp(blend.op);
    target.RenderTargetWriteMask = blend.color_write_mask;
    // normal_write: this draw writes SV_Target1 (G-buffer normals or object motion).
    if (second_target) {
        desc.RenderTarget[1] = target;
        desc.RenderTarget[1].BlendEnable = FALSE;
        desc.RenderTarget[1].RenderTargetWriteMask = normal_write ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
    }

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
    entry.alpha_to_coverage = alpha_to_coverage;
    entry.normal_write = normal_write;
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

bool smoothNormalsEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("RECOMP_D3D_SMOOTH_NORMALS");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

/* Bytes [skip, skip+size) of a draw's vertex allocation; the default fills it all. */
struct VertexUpload {
    const void *bytes;
    UINT skip = 0u, size = UINT_MAX;
};

void evictSmoothNormals(SmoothNormalsCache &cache, std::list<SmoothNormalsEntry>::iterator entry)
{
    for (auto [it, end] = cache.slots.equal_range(entry->slot); it != end; ++it) {
        if (it->second == entry) {
            cache.slots.erase(it);
            break;
        }
    }
    cache.bytes -= entry->bytes();
    cache.entries.erase(entry);
    ++cache.stats.evictions;
}

VertexUpload smoothNormalVertices(RecompD3dPresenter *presenter,
    const RecompD3dPresenterDrawCommand &draw, const RecompD3dVertexLayout &layout,
    bool enabled = smoothNormalsEnabled())
{
    if (!enabled || !draw.directional.enabled || draw.program_count || layout.pretransformed ||
        layout.normal_offset == RECOMP_D3D_FVF_ABSENT ||
        draw.primitive_type < 5u || draw.primitive_type > 7u) return {draw.vertex_bytes};

    static const bool measure = std::getenv("RECOMP_PERF_COUNTER") != nullptr;
    const auto start = measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto &cache = presenter->smooth_normals;
    const auto *bytes = static_cast<const uint8_t *>(draw.vertex_bytes);
    const auto *indices = static_cast<const uint16_t *>(draw.index_bytes);
    const size_t index_size = size_t(draw.index_count)*2u;
    // Guest buffers carry no dirty signal, so a hit compares every referenced
    // byte; CPU-skinned streams and in-place rewrites must miss. The slot only
    // narrows the comparison to one guest buffer and draw shape.
    const uint32_t shape[] = {draw.vertex_address, draw.vertex_count, draw.index_count,
        draw.fvf, draw.vertex_stride, draw.primitive_type};
    uint64_t slot = 0xcbf29ce484222325ull;
    for (const uint32_t value : shape) slot = (slot ^ value) * 0x100000001b3ull;
    // Pose replay submits five vertex sets from one guest buffer.
    constexpr size_t slot_entries = 8u;
    size_t candidates = 0;
    auto found = cache.entries.end(), oldest = found;
    for (auto [it, end] = cache.slots.equal_range(slot); it != end; ++it, ++candidates) {
        const SmoothNormalsEntry &entry = *it->second;
        // Equal indices imply the same referenced range, which lies inside vertex_count.
        if (entry.address == draw.vertex_address && entry.vertex_count == draw.vertex_count &&
            entry.fvf == draw.fvf && entry.stride == draw.vertex_stride &&
            entry.primitive == draw.primitive_type && entry.indices.size() == draw.index_count &&
            !std::memcmp(entry.indices.data(), indices, index_size) &&
            !std::memcmp(entry.source.data(), bytes+size_t(entry.first)*draw.vertex_stride, entry.source.size())) {
            found = it->second;
            break;
        }
        if (oldest == cache.entries.end() || entry.used < oldest->used) oldest = it->second;
    }
    if (found != cache.entries.end()) {
        ++cache.stats.hits;
        found->used = ++cache.clock;
        cache.entries.splice(cache.entries.end(), cache.entries, found);
    } else {
        uint32_t first = UINT32_MAX, last = 0;
        for (uint32_t i=0; i<draw.index_count; ++i) {
            first = (std::min)(first, uint32_t(indices[i]));
            last = (std::max)(last, uint32_t(indices[i]));
        }
        if (last >= draw.vertex_count) return {draw.vertex_bytes};
        ++cache.stats.misses;
        // Index buffers often select only the tail of a shared vertex buffer.
        // Unreferenced prefixes must not consume the cache or invalidate on animation.
        const uint32_t count = last-first+1u;
        const auto *source = bytes+size_t(first)*draw.vertex_stride;
        const size_t size = size_t(count)*draw.vertex_stride;
        const auto rebuild_start = measure ? std::chrono::steady_clock::now() : start;
        auto vertices = recomp_d3d_smooth_normals(source, count, draw.vertex_stride,
            layout, indices, draw.index_count, draw.primitive_type, first);
        if (measure) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-rebuild_start).count();
            cache.stats.rebuild_ms += ms;
            cache.max_rebuild_ms = (std::max)(cache.max_rebuild_ms, ms);
        }
        // Leave room for a full scene plus changing animation buffers.
        constexpr size_t budget = 32u*1024u*1024u;
        const size_t cost = size*2u+index_size;
        if (!vertices.empty() && cost <= budget) {
            // Stale frames of a CPU-animated buffer would otherwise each cost a comparison.
            if (candidates >= slot_entries) evictSmoothNormals(cache, oldest);
            while (!cache.entries.empty() && (cache.bytes+cost > budget || cache.entries.size() >= 1024u))
                evictSmoothNormals(cache, cache.entries.begin());
            cache.entries.push_back({slot, ++cache.clock, draw.vertex_address, draw.vertex_count, first,
                draw.fvf, draw.vertex_stride, draw.primitive_type, {source, source+size},
                std::move(vertices), {indices, indices+draw.index_count}});
            found = std::prev(cache.entries.end());
            cache.slots.emplace(slot, found);
            cache.bytes += cost;
        }
    }
    VertexUpload result{draw.vertex_bytes};
    // Indices keep their base, so only the referenced range of the allocation is filled.
    if (found != cache.entries.end()) result = {found->vertices.data(),
        UINT(found->first*draw.vertex_stride), UINT(found->vertices.size())};
    if (measure) cache.stats.total_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now()-start).count();
    return result;
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

    if (presenter->sun.valid && !presenter->sun.casting && sunGameShadow(draw)) {
        presenter->sun.seen_alpha = (std::max)(presenter->sun.seen_alpha, (draw.texture_factor >> 24) / 255.0f);
        // The shadow map replaces the darkening only where its receivers draw; keep the stencil writes.
        if (presenter->sun.game_alpha > 0 && presenter->sun.epoch == presenter->sun.camera_epoch &&
            sunSameCamera(presenter->sun, draw)) {
            ++presenter->sun.game_shadows;
            auto stencil_only = draw;
            stencil_only.blend.color_write_mask = 0;
            return submitDraw(presenter, stencil_only);
        }
    }
    if (presenter->sun.valid && !presenter->sun.casting && sunOverlayDraw(draw)) {
        SunOverlay shadow = SunOverlay::None;
        if (lookupTexture(presenter, draw, &shadow) && shadow == SunOverlay::Palm) {
            ++presenter->sun.prop_shadows;
            auto stencil_only = draw;
            stencil_only.blend.color_write_mask = 0;
            return submitDraw(presenter, stencil_only);
        }
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
    case RECOMP_D3D_PT_LINELIST:
        if (draw.index_count < 2u || draw.index_count % 2u != 0u)
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
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
    const bool needs_texture = !draw.has_combiner || recomp_d3d_combiner_uses_texture(draw.combiner[0]);
    if (draw.has_texture && needs_texture) {
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
    const VertexUpload vertices = smoothNormalVertices(presenter, draw, layout);
    if (!uploadRing(presenter, &presenter->draw_vertex_buffer,
            presenter->draw_vertex_capacity, presenter->draw_vertex_used,
            vertices.bytes, vertex_size, D3D11_BIND_VERTEX_BUFFER, vertex_offset,
            vertices.skip, vertices.size)) {
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
        draw.has_texture && needs_texture ? lookupTexture(presenter, draw) : nullptr;
    if (needs_texture && (draw.texture_is_frontbuffer || draw.texture_is_backbuffer) &&
        texture_view == nullptr)
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    /* A second cache lookup can evict the first entry. Keep its view alive
       until the context takes its own reference. */
    const bool has_second_texture = draw.has_alpha_mask || draw.has_reflection ||
        draw.program_alpha_mask || draw.has_combiner;
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
    if (draw.has_reflection || draw.program_alpha_mask || draw.has_combiner) {
        if (draw.has_alpha_mask || draw.four_tap_filter || layout.pretransformed ||
            (draw.program_alpha_mask && layout.normal_offset == RECOMP_D3D_FVF_ABSENT) ||
            layout.texcoord_count == 0u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        RecompD3dPresenterDrawCommand reflection{};
        reflection.texture = draw.reflection_texture;
        reflection.texture_bytes = draw.reflection_bytes;
        reflection.texture_byte_count = draw.reflection_byte_count;
        reflection.texture_is_backbuffer = draw.has_combiner && draw.combiner_is_backbuffer;
        reflection.texture_is_frontbuffer = draw.has_combiner && draw.combiner_is_frontbuffer;
        const bool needs_mask = !draw.has_combiner || recomp_d3d_combiner_uses_texture(draw.combiner[1]);
        if (draw.has_combiner && needs_mask) {
            const RenderTargetEntry *sampled = findRenderTarget(presenter, reflection.texture);
            if (sampled != nullptr && sampled->render_view == color_view)
                return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }
        mask_view = needs_mask ? lookupTexture(presenter, reflection) : nullptr;
        if ((needs_mask && mask_view == nullptr) ||
            (texture_view == nullptr && needs_texture))
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.four_tap_filter && texture_view == nullptr) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.program_count) dumpProgramTexture(presenter, texture_view, draw.program_alpha_mask);
    /* Observation only: bind counts say what the guest selected, not what a
       draw actually consumed, and only the latter can explain the frame. */
    if (!presenter->sun.casting) recompD3dPresenterCountDrawTexture(draw, texture_view != nullptr);
    float draw_constants[kDrawConstantFloats]{};
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
        std::memcpy(constants+300, light.specular, sizeof light.specular);
        std::memcpy(constants+332, light.view, sizeof light.view);
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
        float *fog = draw_constants + 140 + 192 * 4 + 336;
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
    /* Spare directional_flags.w: dither only opaque writes to the main target. */
    draw_constants[140 + 192 * 4 + 139] =
        !draw.blend.blend_enable && !draw.target.offscreen ? 1.0f : 0.0f;
    draw_constants[64] = texture_view != nullptr ? 1.0f : 0.0f;
    const bool alpha_to_coverage = !presenter->sun.casting && draw.depth.alpha_test_enable &&
        (draw.depth.alpha_func == RECOMP_D3D_COMPARE_GREATER ||
         draw.depth.alpha_func == RECOMP_D3D_COMPARE_GREATER_EQUAL) &&
        !draw.blend.blend_enable && !draw.target.offscreen && presenter->msaa > 1u &&
        presenter->device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_10_1;
    /* draw_flags.y: 0 disabled, 1 hard alpha test, otherwise sample count. */
    draw_constants[65] = alpha_to_coverage ? static_cast<float>(presenter->msaa) : draw.depth.alpha_test_enable ? 1.0f : 0.0f;
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
    if (draw.has_combiner) {
        std::memcpy(draw_constants + 116, draw.reflection_transform, 64u);
        draw_constants[136] = 3.0f;
        draw_constants[137] = draw.reflection_texture.format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8
            ? 1.0f : 0.0f;
        draw_constants[78] = draw.reflection_texture.linear && draw.reflection_texture.width
            ? 1.0f / draw.reflection_texture.width : 1.0f;
        draw_constants[79] = draw.reflection_texture.linear && draw.reflection_texture.height
            ? 1.0f / draw.reflection_texture.height : 1.0f;
        for (unsigned i = 0u; i < 16u; ++i) {
            draw_constants[140 + 192 * 4 + 336 + 76 + i] =
                static_cast<float>(draw.combiner[i / 8u][i % 8u]);
        }
    }
    /* Temporal inputs: depth-tested draws into the main target are the 3D
       scene. They take the frame's jitter, and the one with the most indices
       names the camera that motion vectors reproject with. */
    const bool scene_draw = presenter->motion_requested && !presenter->sun.casting &&
        (!presenter->dlss_active || presenter->scene_phase) &&
        !draw.target.offscreen && !layout.pretransformed && draw.depth.depth_test_enable &&
        depth_view == presenter->depth_view;
    if (scene_draw) {
        draw_constants[kJitterConstant] = presenter->jitter[0];
        draw_constants[kJitterConstant + 1] = presenter->jitter[1];
        presenter->scene_pending = true;
        bool finite = true;
        for (float value : draw.view_projection) finite &= std::isfinite(value);
        if (!draw.program_count && finite && draw_index_count > presenter->camera_weight) {
            presenter->camera_weight = draw_index_count;
            std::memcpy(presenter->camera, draw.view_projection, sizeof presenter->camera);
        }
    }
    /* Object motion: last frame's draw of the same mesh, nearest in world
       position, supplies its previous world and camera matrices. */
    bool object_write = false;
    if (scene_draw && presenter->object_motion_view != nullptr && !draw.program_count &&
        draw.has_transform && draw.vertex_address != 0u && draw.blend_weight_count < 4u) {
        const uint64_t mesh = (static_cast<uint64_t>(draw.vertex_address) << 32) ^
            (static_cast<uint64_t>(draw.index_count) * 0x9E3779B1u) ^
            (static_cast<uint64_t>(draw.vertex_count) << 12) ^ draw.fvf;
        const uint32_t matrices = draw.blend_weight_count + 1u;
        const RecompD3dPresenter::ObjectHistory *match = nullptr;
        const auto previous = presenter->objects_previous.find(mesh);
        if (previous != presenter->objects_previous.end()) {
            float nearest = INFINITY;
            for (const auto &candidate : previous->second) {
                if (candidate.matrices != matrices) continue;
                float distance = 0.0f;
                for (int axis = 12; axis < 15; ++axis) {
                    const float delta = candidate.world[0][axis] - draw.world[0][axis];
                    distance += delta * delta;
                }
                if (distance < nearest) { nearest = distance; match = &candidate; }
            }
        }
        if (match != nullptr) {
            for (uint32_t i = 0u; i < matrices; ++i) {
                multiply4x4(match->world[i], match->view_projection,
                    draw_constants + kPreviousWvpConstant + 16u * i);
            }
            draw_constants[kMotionFlagsConstant] = 1.0f;
            draw_constants[kMotionFlagsConstant + 2] = static_cast<float>(sceneWidth(presenter));
            draw_constants[kMotionFlagsConstant + 3] = static_cast<float>(sceneHeight(presenter));
            // Opaque surfaces only; blended ones would overwrite what shows through them.
            object_write = draw.depth.depth_write_enable && !draw.blend.blend_enable;
        }
        if (presenter->objects_recorded < 16384u) {
            ++presenter->objects_recorded;
            RecompD3dPresenter::ObjectHistory history{};
            std::memcpy(history.world, draw.world, sizeof(float) * 16u * matrices);
            std::memcpy(history.view_projection, draw.view_projection, sizeof history.view_projection);
            history.matrices = matrices;
            presenter->objects_current[mesh].push_back(history);
        }
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
    const bool anisotropic = draw.linear_mip_filter && draw.has_texture &&
        draw.texture.mip_levels > 1u &&
        !layout.pretransformed && !draw.texture.linear &&
        !draw.texture.render_target && !draw.texture_is_backbuffer &&
        draw.texture.format_byte != RECOMP_D3D_TEXTURE_FORMAT_P8 &&
        !draw.has_reflection && findRenderTarget(presenter, draw.texture) == nullptr;
    ID3D11SamplerState *samplers[] = {
        draw.four_tap_filter || draw.has_alpha_mask || draw.program_count
            ? presenter->filter_sampler
            : lookupDrawSampler(presenter, draw.address_u, draw.address_v, anisotropic),
        draw.program_alpha_mask ? presenter->program_mask_sampler
            : draw.has_combiner
            ? lookupDrawSampler(presenter, draw.combiner_address_u, draw.combiner_address_v, false)
            : presenter->filter_sampler};
    presenter->context->PSSetSamplers(0u, 2u, samplers);
    if (draw.has_combiner && (draw.combiner_lod_bias != 0.0f || draw.combiner_mip_point) &&
        samplers[1] != nullptr) {
        /* The title sea biases its tiled shimmer two mips sharper; D3D11
           returns the existing object for a repeated description. */
        D3D11_SAMPLER_DESC desc{};
        samplers[1]->GetDesc(&desc);
        desc.MipLODBias = draw.combiner_lod_bias;
        if (draw.combiner_mip_point) desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        ID3D11SamplerState *biased = nullptr;
        if (SUCCEEDED(presenter->device->CreateSamplerState(&desc, &biased))) {
            presenter->context->PSSetSamplers(1u, 1u, &biased);
            biased->Release();
        }
    }
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
        const bool normal_write = presenter->gbuffer.enabled && !presenter->sun.casting &&
            !draw.target.offscreen && depth_view == presenter->depth_view && !draw.program_count &&
            !layout.pretransformed && layout.normal_offset != RECOMP_D3D_FVF_ABSENT &&
            draw.directional.enabled && draw.depth.depth_test_enable && draw.depth.depth_write_enable &&
            draw.blend.color_write_mask != 0;
        ID3D11BlendState *blend_state = lookupBlendState(presenter, draw.blend, alpha_to_coverage, normal_write || object_write);
        if (blend_state == nullptr) {
            return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }

        presenter->context->OMSetBlendState(
            blend_state,
            blend_factor,
            0xffffffffu);
    }
    auto &sun = presenter->sun;
    if (sun.enabled && sun.constants) {
        float constants[40]{};
        std::memcpy(constants, sun.projection, sizeof sun.projection);
        // Ground darkens only where the game draws its own shadow, by that shadow's opacity, so scenes
        // without one (hotel, menus) stay untouched. Lit models (character pieces, ball, posts) receive
        // occluder shadows only (see sunShaderSource): character limbs are separately lit pieces whose
        // normals break at the joints, so no per-normal sun term is applied to them.
        const size_t index = sun.draw_index - 1;
        const bool model = draw.blend_weight_count || (index < sun.compact.size() && sun.compact[index]);
        const bool lit_ground = layout.normal_offset != RECOMP_D3D_FVF_ABSENT &&
            layout.diffuse_offset == RECOMP_D3D_FVF_ABSENT && draw.directional.enabled;
        const bool lit_model = model && lit_ground;
        // Grass and its sand fringe are unlit with baked vertex colors; shadows must cross onto them too.
        const bool unlit_ground = !draw.directional.enabled && draw.depth.depth_write_enable &&
            (!draw.blend.blend_enable || (draw.blend.src_factor == RECOMP_D3D_BLEND_SRC_ALPHA &&
             draw.blend.dst_factor == RECOMP_D3D_BLEND_INV_SRC_ALPHA));
        constants[16] = !sun.casting && sun.valid && sun.epoch == sun.camera_epoch &&
            !draw.target.offscreen && !draw.target.no_depth && !draw.program_count &&
            !layout.pretransformed && (((lit_ground || unlit_ground) && !model) || lit_model) &&
            sun.game_alpha > 0 && sunSameCamera(sun, draw) ? 1.0f : 0.0f;
        constants[17] = sun.game_alpha;
        constants[18] = sun.casting ? 1.0f : 0.0f;
        constants[19] = lit_model ? 1.0f : 0.0f;
        std::memcpy(constants + 20, draw.world[0], sizeof draw.world[0]);
        for (unsigned i = 0; i < 3; ++i) constants[36 + i] = -sun.direction[i];
        if (!uploadBuffer(presenter, sun.constants, constants, sizeof constants)) {
            sun.valid = false;
            return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }
        presenter->context->VSSetConstantBuffers(1, 1, &sun.constants);
        presenter->context->PSSetConstantBuffers(1, 1, &sun.constants);
        ID3D11ShaderResourceView *view = sun.casting ? nullptr : sun.view;
        presenter->context->PSSetShaderResources(2, 1, &view);
        presenter->context->PSSetSamplers(2, 1, &sun.sampler);
        if (sun.casting) {
            presenter->context->OMSetRenderTargets(0, nullptr, sun.depth);
            const D3D11_VIEWPORT viewport = {0, 0, float(kSunMapSize), float(kSunMapSize), 0, 1};
            presenter->context->RSSetViewports(1, &viewport);
            presenter->context->RSSetState(sun.rasterizer);
        }
    }
    presenter->context->DrawIndexed(draw_index_count, 0u, 0);
    if (sun.casting) return RECOMP_D3D_PRESENTER_OK;
    if (sun.enabled && draw.target.offscreen && sunPalmTexture(draw.target.color))
        if (auto *entry = findRenderTarget(presenter, draw.target.color)) entry->sun_palm.draw(draw);
#ifdef RECOMP_RTGI
    /* Render-target and backbuffer views change within a frame, so their
       draws trace as opaque. */
    if (presenter->rtgi != nullptr && !presenter->rtgi_applied && !recomp_tuning.rtgi_off &&
        d3d_rtgi_add_draw(presenter->rtgi, draw,
            draw.texture_is_backbuffer || draw.texture.render_target ? nullptr : texture_view,
            draw.has_alpha_mask && !draw.alpha_mask.render_target ? mask_view : nullptr,
            draw.plain_texture_material)) {
        presenter->rtgi_pending = true;
    }
#endif
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
UINT fixedRefreshInterval(UINT hz, UINT rate = 60u)
{
    // Rates below the multiple cannot retire the guest's frames per second.
    const UINT k = (hz + rate / 2u) / rate;
    return (k == 2u || k == 4u) && hz >= rate * k && hz <= rate * k + 1u ? k : 1u;
}

// Whole-game high frame rate presents the passes per second that currently fit.
UINT presentationRate()
{
    return recomp_d3d_display_rate();
}

UINT syncInterval(RecompD3dPresenter *presenter)
{
    if (split_presentation) return 1u;
    const UINT rate = presentationRate();
    const auto now = std::chrono::steady_clock::now();
    if (now >= presenter->next_refresh_check || rate != presenter->sync_rate) {
        presenter->next_refresh_check = now + std::chrono::seconds(1);
        presenter->sync_rate = rate;
        MONITORINFOEXW monitor{};
        monitor.cbSize = sizeof monitor;
        DEVMODEW mode{};
        mode.dmSize = sizeof mode;
        UINT interval = 1u;
        if (GetMonitorInfoW(MonitorFromWindow(presenter->window, MONITOR_DEFAULTTONEAREST), &monitor) &&
            EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
            interval = fixedRefreshInterval(mode.dmDisplayFrequency, rate);
        }
        presenter->sync_interval = interval;
    }
    return presenter->sync_interval;
}

double steadyMs()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void beginFrameCost(RecompD3dPresenter *presenter)
{
    presenter->frame_timing = true;
    presenter->frame_start_ms = steadyMs();
    auto &timer = presenter->gpu_timers[presenter->gpu_timer_next % 4u];
    if (timer.disjoint == nullptr) {
        D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0u};
        presenter->device->CreateQuery(&desc, &timer.disjoint);
        desc.Query = D3D11_QUERY_TIMESTAMP;
        presenter->device->CreateQuery(&desc, &timer.begin);
        presenter->device->CreateQuery(&desc, &timer.end);
    }
    if (timer.disjoint != nullptr && timer.begin != nullptr && timer.end != nullptr) {
        presenter->context->Begin(timer.disjoint);
        presenter->context->End(timer.begin);
    }
}

void endFrameCost(RecompD3dPresenter *presenter)
{
    if (!presenter->frame_timing) return;
    presenter->frame_timing = false;
    const double cpu_ms = steadyMs() - presenter->frame_start_ms;
    auto *context = presenter->context;
    auto &timer = presenter->gpu_timers[presenter->gpu_timer_next++ % 4u];
    if (timer.disjoint != nullptr && timer.begin != nullptr && timer.end != nullptr) {
        context->End(timer.end);
        context->End(timer.disjoint);
    }
    const auto &oldest = presenter->gpu_timers[presenter->gpu_timer_next % 4u];
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    UINT64 begin = 0u, end = 0u;
    constexpr UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    if (presenter->gpu_timer_next >= 4u && oldest.disjoint != nullptr &&
        oldest.begin != nullptr && oldest.end != nullptr &&
        context->GetData(oldest.disjoint, &disjoint, sizeof disjoint, flags) == S_OK &&
        context->GetData(oldest.begin, &begin, sizeof begin, flags) == S_OK &&
        context->GetData(oldest.end, &end, sizeof end, flags) == S_OK &&
        !disjoint.Disjoint && disjoint.Frequency != 0u && end >= begin) {
        presenter->gpu_frame_ms = (end - begin) * 1000.0 / disjoint.Frequency;
        presenter->cost_gpu_ms += presenter->gpu_frame_ms;
        ++presenter->cost_gpu_frames;
    }
    presenter->cost_cpu_ms += cpu_ms;
    ++presenter->cost_frames;
    recomp_d3d_set_frame_cost_ns(static_cast<uint32_t>((std::min)(cpu_ms, 4000.0) * 1e6));
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
#ifdef RECOMP_RTGI
/* Lights the frame's 3D scene once, before its screen-space UI. The GI pass
   clears context state; every draw and clear rebinds what it uses. */
void applyRtgi(RecompD3dPresenter *presenter)
{
    RecompD3dTextureDesc desc{};
    desc.format_byte = 0x12u;
    desc.linear = true;
    desc.width = presenter->config.width;
    desc.height = presenter->config.height;
    ID3D11ShaderResourceView *color = lookupBackBufferTexture(presenter, desc);
    if (color != nullptr) {
        d3d_rtgi_apply(presenter->rtgi, presenter->context, presenter->render_target_view,
            color, presenter->depth_sample, presenter->msaa);
    }
    presenter->rtgi_pending = false;
    presenter->rtgi_applied = true;
}
#endif

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
        if (presenter->resize_pending && !resizeSwapChain(presenter)) {
            return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }
    }

    /* SyncInterval, not the flag, is what paces a present. Passing 0 here
       unconditionally meant the throttled path was never actually throttled:
       every frame was retired immediately and only the blocking behaviour
       changed. Pace to one refresh unless immediate presenting is asked for. */
#ifdef RECOMP_RTGI
    if (presenter->rtgi_pending) applyRtgi(presenter);
    presenter->rtgi_applied = false;
#endif
    if (presenter->gbuffer.enabled) presenter->gbuffer.resolveFrame(presenter->context, presenter->depth_sample);
    finishScene(presenter);  // frames without screen-space UI
    // Before the output pass: its back-buffer write waits for the display.
    endFrameCost(presenter);
    auto *output = presenter->vrr_target_view != nullptr ? presenter->vrr_target_view : presenter->present_target_view;
    if (presenter->gbuffer.enabled) {
        const unsigned cycles = recomp_tuning.gbuffer_cycles.load();
        presenter->gbuffer.debug_view = (presenter->gbuffer.debug_view + cycles - presenter->gbuffer.cycles_seen) % 3u;
        presenter->gbuffer.cycles_seen = cycles;
    }
    if (presenter->gbuffer.enabled && presenter->gbuffer.debug_view) {
        presenter->gbuffer.show(presenter->context, output);
    } else if (!renderOutput(presenter, output)) {
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    copyFrontBuffer(presenter);
    drawMotionDebug(presenter, output);  // after the copy the game reads back
    endMotionFrame(presenter);
    if (presenter->vrr_target_view != nullptr) scaleOutput(presenter);
    // A held frame updates the front buffer the game reads back, but the
    // window keeps the last picture presented.
    if (present.hold) return RECOMP_D3D_PRESENTER_OK;
    {
        if (recomp_screenshot_take_request()) saveScreenshot(presenter);
        // The runtime's overlay text goes on the window's back buffer, after
        // the front buffer the game reads back has been copied.
        RecompOverlayText overlay;
        recomp_overlay_take(&overlay);
        if (!recomp_overlay_empty(&overlay)) {
            ID3D11Texture2D *back_buffer = nullptr;
            presenter->swap_chain->GetBuffer(0u, IID_PPV_ARGS(&back_buffer));
            if (back_buffer != nullptr) {
                D3D11_TEXTURE2D_DESC window{};
                back_buffer->GetDesc(&window);
                releaseCom(back_buffer);
                recomp_overlay_d3d11_draw(presenter->device, presenter->context,
                    presenter->present_target_view, window.Width, window.Height, &overlay);
            }
        }
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
    // Unlocked never waits for vblank. Without tearing support the flip model
    // still shows the newest finished frame at each refresh instead of tearing.
    const UINT sync_interval = immediate || presenter->vrr || unlocked_present ? 0u : syncInterval(presenter);
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
    presenter->replacements.finishFrame(presenter->device, presenter->context, presenter->present_count);
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
            char title[128];
            const int length = std::snprintf(title, sizeof title,
                "DOAXBV Recomp | %.1f FPS | %.1f ms/frame", fps, frame_ms);
#ifdef RECOMP_RTGI
            if (presenter->rtgi != nullptr) {
                std::snprintf(title + length, sizeof title - length, " | RTGI %s %.2f%s",
                    recomp_tuning.rtgi_off ? "off" : "on", recomp_tuning.rtgi_strength.load(),
                    recomp_tuning.rtgi_split ? " split" : "");
            }
#endif
            SetWindowTextA(presenter->window, title);
            // A late frame took over 1.5x this second's average: visible judder.
            double max_ms = 0.0;
            unsigned late = 0u;
            for (const double gap : presenter->present_gaps) {
                max_ms = (std::max)(max_ms, gap);
                late += gap > frame_ms * 1.5;
            }
            char smooth[256] = "";
            if (smoothNormalsEnabled()) {
                auto &s = presenter->smooth_normals;
                std::snprintf(smooth, sizeof smooth, " smooth_hits=%llu smooth_misses=%llu "
                    "smooth_evictions=%llu smooth_ms=%.2f smooth_rebuild_ms=%.2f smooth_max_rebuild_ms=%.2f "
                    "smooth_entries=%zu smooth_cache_mb=%.1f",
                    static_cast<unsigned long long>(s.stats.hits-s.reported.hits),
                    static_cast<unsigned long long>(s.stats.misses-s.reported.misses),
                    static_cast<unsigned long long>(s.stats.evictions-s.reported.evictions),
                    s.stats.total_ms-s.reported.total_ms, s.stats.rebuild_ms-s.reported.rebuild_ms,
                    s.max_rebuild_ms, s.entries.size(), s.bytes/1048576.0);
                s.reported = s.stats;
                s.max_rebuild_ms = 0;
            }
            char rtgi_costs[384] = "";
#ifdef RECOMP_RTGI
            if (presenter->rtgi != nullptr) {
                /* CPU per GI pass; GPU per timed pass. Traced passes rebuild and trace
                   the scene; reused passes reproject the last traced GI. */
                const D3dRtgiStats s = d3d_rtgi_take_stats(presenter->rtgi);
                const double passes = (std::max)(1u, s.traced + s.reused);
                const double d12 = (std::max)(1u, s.d3d12Passes), d11 = (std::max)(1u, s.d3d11Passes);
                std::snprintf(rtgi_costs, sizeof rtgi_costs,
                    " rtgi_traced=%u rtgi_refitted=%u rtgi_reused=%u rtgi_tris=%.0f"
                    " rtgi_cpu_ms=collect:%.2f,apply:%.2f,wait:%.2f"
                    " rtgi_gpu_ms=build:%.2f,trace:%.2f,temporal:%.2f,spatial:%.2f,composite:%.2f,sync:%.2f",
                    s.traced, s.refitted, s.reused, double(s.triangles) / (std::max)(1u, s.traced),
                    s.collectMs / passes, s.applyMs / passes, s.waitMs / passes,
                    s.buildMs / d12, s.traceMs / d12, s.temporalMs / d12, s.spatialMs / d12,
                    s.compositeMs / d11, s.syncMs / d11);
            }
#endif
            unsigned passes[3];
            recomp_d3d_take_passes(passes);
            std::fprintf(stderr, "recomp performance: tick_ms=%llu present=%u fps=%.2f frame_ms=%.3f "
                "max_ms=%.1f late=%u present_max_ms=%.1f holds=%u,%u,%u,%u,%u,%u,%u,%u,%u "
                "ticks_s=%u passes_s=%u dropped_s=%u render_cpu_ms=%.2f render_gpu_ms=%.2f%s%s\n",
                static_cast<unsigned long long>(now), presenter->present_count, fps, frame_ms,
                max_ms, late, presenter->present_call_max_ms,
                presenter->refresh_holds[0], presenter->refresh_holds[1], presenter->refresh_holds[2],
                presenter->refresh_holds[3], presenter->refresh_holds[4], presenter->refresh_holds[5],
                presenter->refresh_holds[6], presenter->refresh_holds[7], presenter->refresh_holds[8],
                passes[0], passes[1], passes[2],
                presenter->cost_frames ? presenter->cost_cpu_ms / presenter->cost_frames : 0.0,
                presenter->cost_gpu_frames ? presenter->cost_gpu_ms / presenter->cost_gpu_frames : 0.0,
                smooth, rtgi_costs);
            presenter->cost_cpu_ms = presenter->cost_gpu_ms = 0.0;
            presenter->cost_frames = presenter->cost_gpu_frames = 0u;
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
    if (const char *mode = std::getenv("RECOMP_D3D_WINDOW_MODE")) {
        if (std::strcmp(mode, "windowed") == 0) created->window_mode = WindowMode::Windowed;
        if (std::strcmp(mode, "borderless") == 0) created->window_mode = WindowMode::Borderless;
    }
    if (const char *height = std::getenv("RECOMP_D3D_WINDOW_HEIGHT")) {
        created->window_height_request = static_cast<uint32_t>(std::clamp(std::atoi(height), 0, 8640));
    }
    if (const char *upscale = std::getenv("RECOMP_D3D_UPSCALE")) {
        created->fsr_requested = std::strcmp(upscale, "fsr") == 0;
    }
    if (const char *sharpness = std::getenv("RECOMP_D3D_SHARPNESS")) {
        const float value = static_cast<float>(std::atof(sharpness));
        if (std::isfinite(value)) created->fsr_sharpness = std::clamp(value, 0.0f, 1.0f);
    }
    const auto flag = [](const char *name) {
        const char *value = std::getenv(name);
        return value != nullptr && std::strcmp(value, "1") == 0;
    };
    created->motion_debug = flag("RECOMP_D3D_MOTION_DEBUG");
    created->jitter_enabled = flag("RECOMP_D3D_JITTER");
    if (const char *dlss = std::getenv("RECOMP_D3D_DLSS")) {
        static const char *modes[] = {"dlaa", "quality", "balanced", "performance", "ultra"};
        for (int i = 0; i < 5; ++i) if (std::strcmp(dlss, modes[i]) == 0) created->dlss_mode = i;
    }
    created->jitter_enabled = created->jitter_enabled || created->dlss_mode >= 0;
    created->motion_requested = flag("RECOMP_D3D_MOTION_VECTORS") ||
        created->motion_debug || created->jitter_enabled;
    if (const char *anisotropy = std::getenv("RECOMP_D3D_ANISOTROPY")) {
        const int value = std::atoi(anisotropy);
        created->anisotropy = value >= 2 ? static_cast<uint32_t>((std::min)(value, 16)) : 0u;
    }
    created->sun.enabled = sunShadowRequested();
    created->gbuffer.enabled = gbufferRequested();
    if (created->gbuffer.enabled) {
        const char *view = std::getenv("RECOMP_D3D_GBUFFER_VIEW");
        if (view && std::strcmp(view, "normal") == 0) created->gbuffer.debug_view = 1;
        if (view && std::strcmp(view, "depth") == 0) created->gbuffer.debug_view = 2;
    }
    const char *smaa = std::getenv("RECOMP_D3D_SMAA");
    created->smaa = smaa != nullptr && std::strcmp(smaa, "0") != 0;
    /* VRR shows each frame when it is presented, so any VRR display paces 60 Hz
       evenly. DXGI cannot tell whether VRR is active; without it this tears. */
    const char *vrr = std::getenv("RECOMP_D3D_VRR");
    // An unlocked frame rate presents through the same tearing path.
    if (unlocked_present || (vrr != nullptr && std::strcmp(vrr, "1") == 0)) {
        IDXGIFactory5 *factory = nullptr;
        BOOL tearing = FALSE;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
            factory->Release();
        }
        created->vrr = tearing != FALSE;
        std::fprintf(stderr, "recomp d3d presenter: vrr %s%s\n", created->vrr ? "on" : "unsupported",
            !unlocked_present ? "" : created->vrr ? " (unlocked frame rate)"
                : " (unlocked frame rate without tearing: newest frame at each refresh)");
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
    const char *rtgi = std::getenv("RECOMP_D3D_RTGI");
    created->rtgi_requested = rtgi != nullptr && std::strcmp(rtgi, "1") == 0;
#ifndef RECOMP_RTGI
    if (created->rtgi_requested) {
        std::fprintf(stderr, "recomp rtgi: not built into this runner\n");
        created->rtgi_requested = false;
    }
#endif
    if (created->dlss_mode >= 0) {
        /* DLSS anti-aliases and needs single-sampled inputs; RTGI and the
           G-buffer are sized for one main target set. */
        created->msaa = 1u;
        created->smaa = false;
        created->rtgi_requested = false;
        created->gbuffer.enabled = false;
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
        created->precompile = std::thread(precompileDrawShaders, &created->precompile_stop,
            created->device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_10_1, created->sun.enabled, created->gbuffer.enabled);
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

/* Sun caster depth: blend bones already multiplied by the sun projection, and for
   cutouts the base texture's level-0 alpha against the draw's alpha test. */
constexpr char kSunCasterShader[] = R"(
cbuffer SunCaster : register(b0) {
    row_major float4x4 caster_bones[4];
    float4 caster_alpha; // xy: linear texture scale, z: compare function, w: reference
};
Texture2D caster_texture : register(t0);
SamplerState caster_sampler : register(s0);
struct VSIn {
    float3 position : POSITION;
#if WEIGHTS
    float3 weights : BLENDWEIGHT;
#endif
#if TEXCOORD
    float2 texcoord : TEXCOORD0;
#endif
};
struct VSOut {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};
VSOut vs_main(VSIn input) {
    VSOut output;
    output.position = mul(float4(input.position, 1), caster_bones[0]);
#if WEIGHTS
    output.position = 0;
    float remainder = 1;
    [unroll] for (uint i = 0; i < WEIGHTS; ++i) {
        output.position += input.weights[i] * mul(float4(input.position, 1), caster_bones[i]);
        remainder -= input.weights[i];
    }
    output.position += remainder * mul(float4(input.position, 1), caster_bones[WEIGHTS]);
#endif
#if TEXCOORD
    output.texcoord = input.texcoord;
#else
    output.texcoord = 0;
#endif
    return output;
}
// Mip averages erase thin net wires before the cutoff, so test level 0 as visible draws compare.
void ps_main(VSOut input) {
    float alpha = round(saturate(caster_texture.SampleLevel(caster_sampler, input.texcoord * caster_alpha.xy, 0).a) * 255.0f);
    float ref = caster_alpha.w;
    int func = (int)caster_alpha.z;
    bool alpha_pass = func == 7 ||
        (func == 1 && alpha < ref) ||
        (func == 2 && alpha == ref) ||
        (func == 3 && alpha <= ref) ||
        (func == 4 && alpha > ref) ||
        (func == 5 && alpha != ref) ||
        (func == 6 && alpha >= ref);
    if (!alpha_pass) discard;
}
)";

std::string sunCasterSource(unsigned weights, bool texcoord)
{
    return "#define WEIGHTS " + std::to_string(weights) + "\n#define TEXCOORD " +
        (texcoord ? "1" : "0") + "\n" + kSunCasterShader;
}

/* The vertex shader and input layout for one FVF and bone count, built on first use;
   a failure is remembered so the draw keeps taking submitDraw. */
const SunCasterLayout *lookupSunCaster(RecompD3dPresenter *presenter,
    const RecompD3dPresenterDrawCommand &draw, const RecompD3dVertexLayout &layout)
{
    auto &sun = presenter->sun;
    if (!sun.caster_constants && !sun.caster_failed) {
        sun.caster_failed = true;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 68*sizeof(float);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3DBlob *blob = nullptr;
        const std::string source = sunCasterSource(0, true);
        if (compileDrawShader(source.c_str(), "ps_main", "ps_4_0", &blob) &&
            SUCCEEDED(presenter->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                nullptr, &sun.caster_alpha)) &&
            SUCCEEDED(presenter->device->CreateBuffer(&desc, nullptr, &sun.caster_constants)))
            sun.caster_failed = false;
        releaseCom(blob);
        if (sun.caster_failed) { releaseCom(sun.caster_alpha); releaseCom(sun.caster_constants); }
    }
    if (sun.caster_failed) return nullptr;
    for (const auto &caster : sun.caster_layouts)
        if (caster.fvf == draw.fvf && caster.weights == draw.blend_weight_count)
            return caster.input ? &caster : nullptr;
    SunCasterLayout created{draw.fvf, draw.blend_weight_count, nullptr, nullptr};
    const bool texcoord = layout.texcoord_offset != RECOMP_D3D_FVF_ABSENT;
    D3D11_INPUT_ELEMENT_DESC elements[3]{};
    UINT count = 0u;
    elements[count++] = {"POSITION", 0u, DXGI_FORMAT_R32G32B32_FLOAT, 0u,
        layout.position_offset, D3D11_INPUT_PER_VERTEX_DATA, 0u};
    if (draw.blend_weight_count) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT};
        elements[count++] = {"BLENDWEIGHT", 0u, formats[draw.blend_weight_count - 1u], 0u,
            12u, D3D11_INPUT_PER_VERTEX_DATA, 0u};
    }
    if (texcoord) elements[count++] = {"TEXCOORD", 0u, DXGI_FORMAT_R32G32_FLOAT, 0u,
        layout.texcoord_offset, D3D11_INPUT_PER_VERTEX_DATA, 0u};
    ID3DBlob *blob = nullptr;
    const std::string source = sunCasterSource(draw.blend_weight_count, texcoord);
    if (!compileDrawShader(source.c_str(), "vs_main", "vs_4_0", &blob) ||
        FAILED(presenter->device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
            nullptr, &created.shader)) ||
        FAILED(presenter->device->CreateInputLayout(elements, count, blob->GetBufferPointer(),
            blob->GetBufferSize(), &created.input))) {
        releaseCom(created.input);
        releaseCom(created.shader);
    }
    releaseCom(blob);
    sun.caster_layouts.push_back(created);
    return created.input ? &sun.caster_layouts.back() : nullptr;
}

/* Draws one caster into the sun map with the depth-only pipeline. UNSUPPORTED leaves
   the draw to submitDraw: fans need its index expansion, and alpha masks, four-tap or
   framebuffer textures and alpha tests without a sampled texture need its shader. */
RecompD3dPresenterError submitSunCaster(RecompD3dPresenter *presenter,
    const RecompD3dPresenterDrawCommand &draw, const float bones[4][16])
{
    auto &sun = presenter->sun;
    // Blended casters cut out at half alpha.
    const bool blended = draw.blend.blend_enable;
    const RecompD3dCompareFunc func = blended ? RECOMP_D3D_COMPARE_GREATER : draw.depth.alpha_func;
    const bool alpha_test = (blended || draw.depth.alpha_test_enable) && func != RECOMP_D3D_COMPARE_ALWAYS;
    RecompD3dVertexLayout layout;
    if ((draw.primitive_type != 5u && draw.primitive_type != 6u) || draw.has_alpha_mask ||
        draw.four_tap_filter || draw.texture_is_backbuffer || draw.texture_is_frontbuffer ||
        (alpha_test && !draw.has_texture) || !recomp_d3d_fvf_layout(draw.fvf, &layout) ||
        (alpha_test && layout.texcoord_offset == RECOMP_D3D_FVF_ABSENT) || !ensureSharedDrawState(presenter))
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    const SunCasterLayout *caster = lookupSunCaster(presenter, draw, layout);
    ID3D11ShaderResourceView *texture = caster && alpha_test ? lookupTexture(presenter, draw) : nullptr;
    if (!caster || (alpha_test && !texture)) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    float constants[68]{};
    std::memcpy(constants, bones, 64*sizeof(float));
    constants[64] = draw.texture.linear && draw.texture.width ? 1.0f/draw.texture.width : 1.0f;
    constants[65] = draw.texture.linear && draw.texture.height ? 1.0f/draw.texture.height : 1.0f;
    constants[66] = static_cast<float>(func);
    constants[67] = blended ? 127.0f : static_cast<float>(draw.depth.alpha_ref);
    UINT vertex_offset = 0u, index_offset = 0u;
    if (!uploadRing(presenter, &presenter->draw_vertex_buffer, presenter->draw_vertex_capacity,
            presenter->draw_vertex_used, draw.vertex_bytes, draw.vertex_stride*draw.vertex_count,
            D3D11_BIND_VERTEX_BUFFER, vertex_offset) ||
        !uploadRing(presenter, &presenter->draw_index_buffer, presenter->draw_index_capacity,
            presenter->draw_index_used, draw.index_bytes, draw.index_count*2u,
            D3D11_BIND_INDEX_BUFFER, index_offset) ||
        !uploadBuffer(presenter, sun.caster_constants, constants, sizeof constants))
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    auto *context = presenter->context;
    if (!sun.caster_bound) {
        RecompD3dDepthState depth{};
        depth.depth_test_enable = depth.depth_write_enable = true;
        depth.depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
        ID3D11DepthStencilState *depth_state = lookupDepthState(presenter, depth);
        if (depth_state == nullptr) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        context->OMSetRenderTargets(0, nullptr, sun.depth);
        const D3D11_VIEWPORT viewport = {0, 0, float(kSunMapSize), float(kSunMapSize), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(sun.rasterizer);
        context->OMSetDepthStencilState(depth_state, 0);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        context->VSSetConstantBuffers(0, 1, &sun.caster_constants);
        context->PSSetConstantBuffers(0, 1, &sun.caster_constants);
        sun.caster_bound = true;
    }
    const UINT stride = draw.vertex_stride;
    context->IASetInputLayout(caster->input);
    context->IASetVertexBuffers(0, 1, &presenter->draw_vertex_buffer, &stride, &vertex_offset);
    context->IASetIndexBuffer(presenter->draw_index_buffer, DXGI_FORMAT_R16_UINT, index_offset);
    context->IASetPrimitiveTopology(draw.primitive_type == 6u ?
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(caster->shader, nullptr, 0);
    context->PSSetShader(alpha_test ? sun.caster_alpha : nullptr, nullptr, 0);
    if (alpha_test) {
        ID3D11SamplerState *sampler = lookupDrawSampler(presenter, draw.address_u, draw.address_v);
        context->PSSetShaderResources(0, 1, &texture);
        context->PSSetSamplers(0, 1, &sampler);
    }
    context->DrawIndexed(draw.index_count, 0, 0);
    return RECOMP_D3D_PRESENTER_OK;
}

// Two worker prepasses use the exact same pose replay as visible draws.
bool d3d11_backend_shadow_begin(RecompD3dPresenter *presenter, bool casting)
{
    auto &sun = presenter->sun;
    if (!sun.enabled) return false;
    // The caster prepass is part of this frame's render cost.
    if (!presenter->frame_timing) beginFrameCost(presenter);
    sun.epoch = 0;
    sun.casting = casting;
    if (!casting) {
        sun.valid = false;
        sun.triangles = sun.casters = 0;
        return sunCreate(sun, presenter->device);
    }
    if (sun.map_current) {
        sun.casting = false;
        sun.valid = sun.triangles != 0;
        sun.casters = sun.map_casters;
        return false;
    }
    sun.valid = sun.triangles && sunFit(sun);
    if (!sun.valid) { sun.casting = false; return false; }
    sun.compact.clear();
    ID3D11ShaderResourceView *empty = nullptr;
    presenter->context->PSSetShaderResources(2, 1, &empty);
    presenter->context->ClearDepthStencilView(sun.depth, D3D11_CLEAR_DEPTH, 1, 0);
    return true;
}

void d3d11_backend_shadow_tick(RecompD3dPresenter *presenter)
{
    presenter->sun.map_current = false;
}

void d3d11_backend_shadow_command(RecompD3dPresenter *presenter,
    const RecompD3dPresenterCommand *command)
{
    auto &sun = presenter->sun;
    if (command->type == RECOMP_D3D_PRESENTER_COMMAND_CLEAR && command->data.clear.clear_depth &&
        !command->data.clear.target.offscreen && !command->data.clear.target.no_depth) {
        ++sun.epoch;
        if (!sun.casting) sun.triangles = 0;
    }
    if (command->type != RECOMP_D3D_PRESENTER_COMMAND_DRAW) return;
    const auto &draw = command->data.draw;
    if (!sun.casting) { sunSelectCamera(sun, draw); return; }
    float size[3];
    sun.compact.push_back(0);
    bool net = false;
    if (sun.epoch != sun.camera_epoch || !sunSameCamera(sun, draw) || !sunSceneDraw(draw, size, &net)) return;
    // A terrain-following shadow decal can pass the scenery bounds test.
    SunOverlay overlay = SunOverlay::None;
    if (sunOverlayDraw(draw)) lookupTexture(presenter, draw, &overlay);
    const bool casts = overlay == SunOverlay::None && !net && sunCaster(draw, size);
    sun.compact.back() = !draw.blend_weight_count &&
        (casts || (std::max)({size[0], size[1], size[2]}) <= 4);
    if (!casts) return;
    float bones[4][16]{};
    for (unsigned b = 0; b <= draw.blend_weight_count; ++b)
        for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
            for (unsigned k = 0; k < 4; ++k) bones[b][r*4+c] += draw.world[b][r*4+k]*sun.projection[k*4+c];
    RecompD3dPresenterError result = submitSunCaster(presenter, draw, bones);
    if (result == RECOMP_D3D_PRESENTER_OK) ++sun.casters;
    if (result != RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND) return;
    // Fallback through the shared draw shader for what submitSunCaster leaves out.
    RecompD3dPresenterDrawCommand caster = draw;
    std::memcpy(caster.transform, bones[0], sizeof caster.transform);
    std::memcpy(caster.blend_transforms, bones[1], sizeof caster.blend_transforms);
    caster.directional.enabled = false;
    caster.has_combiner = caster.has_reflection = false;
    caster.depth.depth_write_enable = true;
    if (draw.blend.blend_enable) {
        caster.depth.alpha_test_enable = true;
        caster.depth.alpha_func = RECOMP_D3D_COMPARE_GREATER;
        caster.depth.alpha_ref = 127;
    }
    caster.depth.depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
    caster.depth.stencil_enable = false;
    sun.caster_bound = false;
    if (submitDraw(presenter, caster) == RECOMP_D3D_PRESENTER_OK) ++sun.casters;
}

void d3d11_backend_shadow_end(RecompD3dPresenter *presenter)
{
    auto &sun = presenter->sun;
    if (sun.casting) {
        sun.map_current = sun.casters != 0;
        sun.map_casters = sun.casters;
    }
    sun.casting = false;
    sun.caster_bound = false;
    sun.epoch = 0;
    sun.valid = sun.valid && sun.casters != 0;
    presenter->context->OMSetRenderTargets(0, nullptr, nullptr);
    // game_shadows counts the previous frame's hidden darkening passes (false-match audit).
    if (presenter->performance_counter && presenter->present_count % 120 == 0 && sun.valid)
        std::fprintf(stderr, "recomp sun shadow: present=%u casters=%u game_shadows=%u prop_shadows=%u direction=%.4f,%.4f,%.4f\n",
            presenter->present_count, sun.casters, sun.game_shadows, sun.prop_shadows,
            sun.direction[0], sun.direction[1], sun.direction[2]);
    sun.game_shadows = 0;
    sun.prop_shadows = 0;
    sun.game_alpha = sun.seen_alpha;
    sun.seen_alpha = 0;
    sun.draw_index = 0;
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
    if (!presenter->frame_timing) beginFrameCost(presenter);
    if (command->type == RECOMP_D3D_PRESENTER_COMMAND_DRAW) ++presenter->sun.draw_index;

    switch (command->type) {
    case RECOMP_D3D_PRESENTER_COMMAND_GAMMA:
        return submitGamma(presenter, command->data.gamma);
    case RECOMP_D3D_PRESENTER_COMMAND_CLEAR:
        if (presenter->sun.enabled && command->data.clear.clear_depth &&
            !command->data.clear.target.offscreen && !command->data.clear.target.no_depth)
            ++presenter->sun.epoch;
        return submitClear(presenter, command->data.clear);
    case RECOMP_D3D_PRESENTER_COMMAND_PRESENT:
        presenter->sun.valid = false;
        presenter->sun.triangles = presenter->sun.epoch = 0;
        return submitPresent(presenter, command->data.present);
    case RECOMP_D3D_PRESENTER_COMMAND_DRAW: {
        const RecompD3dPresenterDrawCommand &draw = command->data.draw;
        // The first screen-space draw on the main target ends the 3D scene.
        RecompD3dVertexLayout layout{};
        const bool interface_draw = !draw.target.offscreen &&
            recomp_d3d_fvf_layout(draw.fvf, &layout) && layout.pretransformed;
        if (interface_draw && presenter->scene_pending) finishScene(presenter);
#ifdef RECOMP_RTGI
        if (presenter->rtgi_pending && interface_draw) {
            applyRtgi(presenter);
        }
#endif
        return submitDraw(presenter, draw);
    }
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
    if (std::getenv("RECOMP_PERF_COUNTER") && destroyed->smooth_normals.stats.misses) {
        const auto &s = destroyed->smooth_normals;
        std::fprintf(stderr, "recomp smooth normals: hits=%llu misses=%llu evictions=%llu total_ms=%.3f rebuild_ms=%.3f cache_bytes=%zu presents=%u\n",
            static_cast<unsigned long long>(s.stats.hits), static_cast<unsigned long long>(s.stats.misses),
            static_cast<unsigned long long>(s.stats.evictions), s.stats.total_ms, s.stats.rebuild_ms,
            s.bytes, destroyed->present_count);
    }
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

void d3d11_backend_set_unlocked_present(bool enabled)
{
    unlocked_present = enabled;
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
