#include "d3d_draw_shader.h"

#include <cstdio>

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
void recomp_d3d_draw_shader_source(
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
