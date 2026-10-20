#include "d3d_glsl_shader.h"
#include "d3d_vertex_program.h"

#include <string>

static const char kGlslUniformsVertex[] = R"(
layout(set = 1, binding = 0, std140) uniform TransformUniforms {
    mat4 wvp[4];
    vec4 draw_flags;
    vec4 blend_flags;
    vec4 texture_factor;
    vec4 lighting_flags;
    vec4 texture_flags;
    mat4 reflection_world_view;
    mat4 reflection_normal;
    mat4 reflection_transform;
    vec4 reflection_diffuse;
    vec4 reflection_flags;
    vec4 directional_flags;
    mat4 directional_normals[4];
    vec4 directional_base;
    vec4 directional_material;
    vec4 directional_directions[8];
    vec4 directional_colors[8];
} u;
)";

static const char kGlslUniformsFragment[] = R"(
layout(set = 3, binding = 0, std140) uniform TransformUniforms {
    mat4 wvp[4];
    vec4 draw_flags;
    vec4 blend_flags;
    vec4 texture_factor;
    vec4 lighting_flags;
    vec4 texture_flags;
    mat4 reflection_world_view;
    mat4 reflection_normal;
    mat4 reflection_transform;
    vec4 reflection_diffuse;
    vec4 reflection_flags;
    vec4 directional_flags;
    mat4 directional_normals[4];
    vec4 directional_base;
    vec4 directional_material;
    vec4 directional_directions[8];
    vec4 directional_colors[8];
} u;
)";

std::string recomp_glsl_vertex_shader(const RecompD3dVertexLayout &layout, uint32_t fvf,
                                      uint32_t program_count, const uint32_t (*program_tokens)[4])
{
    const bool has_normal = layout.normal_offset != RECOMP_D3D_FVF_ABSENT;
    const bool has_diffuse = layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT;
    const bool has_texcoord = layout.texcoord_offset != RECOMP_D3D_FVF_ABSENT;
    const bool four_coords = (layout.texcoord_count == 4u);
    const bool two_coords = (layout.texcoord_count == 2u);

    std::string s;
    s.reserve(4096);
    s += "#version 450\n";
    s += kGlslUniformsVertex;
    if (program_count) {
        s += "layout(set = 1, binding = 1, std140) uniform ProgramConstants { vec4 vc[192]; } pc;\n";
    }

    if (layout.pretransformed) {
        s += "layout(location = 0) in vec4 in_position;\n";
    } else {
        s += "layout(location = 0) in vec3 in_position;\n";
    }

    if (layout.blend_weight_count == 1u) {
        s += "layout(location = 1) in float in_weights;\n";
    } else if (layout.blend_weight_count == 2u) {
        s += "layout(location = 1) in vec2 in_weights;\n";
    } else if (layout.blend_weight_count == 3u) {
        s += "layout(location = 1) in vec3 in_weights;\n";
    }

    if (has_normal) s += "layout(location = 2) in vec3 in_normal;\n";
    if (has_diffuse) s += "layout(location = 3) in vec4 in_diffuse;\n";
    if (has_texcoord) s += "layout(location = 4) in vec2 in_texcoord;\n";
    if (four_coords) {
        s += "layout(location = 5) in vec2 in_texcoord1;\n";
        s += "layout(location = 6) in vec2 in_texcoord2;\n";
        s += "layout(location = 7) in vec2 in_texcoord3;\n";
    } else if (two_coords) {
        s += "layout(location = 5) in vec2 in_texcoord1;\n";
    }

    s += "layout(location = 0) out vec4 v_color;\n";
    s += "layout(location = 1) out vec2 v_texcoord;\n";
    s += "layout(location = 2) out vec2 v_reflection_coord;\n";
    if (four_coords) {
        s += "layout(location = 3) out vec2 v_texcoord1;\n";
        s += "layout(location = 4) out vec2 v_texcoord2;\n";
        s += "layout(location = 5) out vec2 v_texcoord3;\n";
    } else if (two_coords) {
        s += "layout(location = 3) out vec2 v_texcoord1;\n";
    }
    if (program_count) {
        s += "layout(location = 6) out vec2 v_program_q;\n";
    }

    s += "void main() {\n";

    bool has_prog = false;
    if (program_count) {
        std::string prog_body;
        if (recomp_d3d_vertex_program_glsl_source(program_tokens, program_count, prog_body)) {
            s += prog_body;
            s += "}\n";
            has_prog = true;
        }
        // Like the D3D11 presenter, skip a draw whose program does not translate.
        if (!has_prog) return {};
    }

    if (!has_prog) {
        if (layout.pretransformed) {
            s += "    gl_Position = (u.wvp[0] * vec4(in_position.xyz, 1.0)) / in_position.w;\n";
        } else if (layout.blend_weight_count != 0u) {
            s += "    if (u.blend_flags.x > 0.5) {\n";
            s += "        vec4 p = vec4(0.0);\n";
            s += "        float remainder = 1.0;\n";
            if (layout.blend_weight_count == 1u) {
                s += "        p += in_weights * (u.wvp[0] * vec4(in_position, 1.0));\n";
                s += "        remainder -= in_weights;\n";
            } else {
                for (uint32_t i = 0u; i < layout.blend_weight_count; ++i) {
                    s += "        p += in_weights[" + std::to_string(i) + "] * (u.wvp[" + std::to_string(i) + "] * vec4(in_position, 1.0));\n";
                    s += "        remainder -= in_weights[" + std::to_string(i) + "];\n";
                }
            }
            s += "        p += remainder * (u.wvp[" + std::to_string(layout.blend_weight_count) + "] * vec4(in_position, 1.0));\n";
            s += "        gl_Position = p;\n";
            s += "    } else {\n";
            s += "        gl_Position = u.wvp[0] * vec4(in_position, 1.0);\n";
            s += "    }\n";
        } else {
            s += "    gl_Position = u.wvp[0] * vec4(in_position, 1.0);\n";
        }

        // Color & lighting
        if (has_diffuse) {
            s += "    v_color = in_diffuse.zyxw;\n";
        } else if (has_normal && !layout.pretransformed) {
            s += "    if (u.directional_flags.x > 0.5) {\n";
            s += "        vec3 n = (u.directional_normals[0] * vec4(in_normal, 0.0)).xyz;\n";
            if (layout.blend_weight_count != 0u) {
                s += "        if (u.blend_flags.x > 0.5) {\n";
                s += "            n = vec3(0.0);\n";
                s += "            float remainder = 1.0;\n";
                if (layout.blend_weight_count == 1u) {
                    s += "            n += in_weights * (u.directional_normals[0] * vec4(in_normal, 0.0)).xyz;\n";
                    s += "            remainder -= in_weights;\n";
                } else {
                    for (uint32_t i = 0u; i < layout.blend_weight_count; ++i) {
                        s += "            n += in_weights[" + std::to_string(i) + "] * (u.directional_normals[" + std::to_string(i) + "] * vec4(in_normal, 0.0)).xyz;\n";
                        s += "            remainder -= in_weights[" + std::to_string(i) + "];\n";
                    }
                }
                s += "            n += remainder * (u.directional_normals[" + std::to_string(layout.blend_weight_count) + "] * vec4(in_normal, 0.0)).xyz;\n";
                s += "        }\n";
            }
            s += "        if (u.directional_flags.y > 0.5 && dot(n, n) > 0.0) n = normalize(n);\n";
            s += "        vec3 rgb = u.directional_base.rgb;\n";
            s += "        uint light_cnt = uint(u.directional_flags.z);\n";
            s += "        for (uint i = 0u; i < light_cnt; ++i) {\n";
            s += "            rgb += u.directional_material.rgb * u.directional_colors[i].rgb * max(0.0, dot(n, u.directional_directions[i].xyz));\n";
            s += "        }\n";
            s += "        v_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n";
            s += "    } else {\n";
            s += "        v_color = vec4(abs(normalize(in_normal)), 1.0);\n";
            s += "    }\n";
        } else {
            s += "    v_color = vec4(0.75, 0.75, 0.78, 1.0);\n";
        }

        s += has_texcoord ? "    v_texcoord = in_texcoord;\n" : "    v_texcoord = vec2(0.0);\n";
        if (four_coords) {
            s += "    v_texcoord1 = in_texcoord1;\n";
            s += "    v_texcoord2 = in_texcoord2;\n";
            s += "    v_texcoord3 = in_texcoord3;\n";
        } else if (two_coords) {
            s += "    v_texcoord1 = in_texcoord1;\n";
        }

        s += "    v_reflection_coord = vec2(0.0);\n";
        if (has_texcoord && !layout.pretransformed) {
            if (has_normal) {
                s += "    if (u.reflection_flags.x > 1.5) {\n";
                s += "        v_reflection_coord = in_texcoord;\n";
                s += "    } else if (u.reflection_flags.x > 0.5) {\n";
                s += "        vec3 eye = (u.reflection_world_view * vec4(in_position, 1.0)).xyz;\n";
                s += "        vec3 n = (u.reflection_normal * vec4(in_normal, 0.0)).xyz;\n";
                s += "        if (u.reflection_flags.y > 0.5) n = normalize(n);\n";
                s += "        vec3 r = reflect(normalize(eye), n);\n";
                s += "        v_reflection_coord = (u.reflection_transform * vec4(r, 1.0)).xy;\n";
                s += "    }\n";
            } else {
                s += "    if (u.reflection_flags.x > 1.5) {\n";
                s += "        v_reflection_coord = in_texcoord;\n";
                s += "    } else if (u.reflection_flags.x > 0.5) {\n";
                s += "        vec3 eye = (u.reflection_world_view * vec4(in_position, 1.0)).xyz;\n";
                s += "        vec3 r = normalize(eye);\n";
                s += "        v_reflection_coord = (u.reflection_transform * vec4(r, 1.0)).xy;\n";
                s += "    }\n";
            }
        }
        s += "}\n";
    }
    return s;
}

std::string recomp_glsl_fragment_shader(const RecompD3dVertexLayout &layout, uint32_t fvf,
                                        uint32_t program_count)
{
    const bool has_diffuse = layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT;
    const bool four_coords = (layout.texcoord_count == 4u);
    const bool two_coords = (layout.texcoord_count == 2u);

    std::string s;
    s.reserve(4096);
    s += "#version 450\n";
    s += kGlslUniformsFragment;
    s += "layout(set = 2, binding = 0) uniform sampler2D guest_texture;\n";
    s += "layout(set = 2, binding = 1) uniform sampler2D alpha_mask;\n";
    s += "layout(location = 0) in vec4 v_color;\n";
    s += "layout(location = 1) in vec2 v_texcoord;\n";
    s += "layout(location = 2) in vec2 v_reflection_coord;\n";
    if (four_coords) {
        s += "layout(location = 3) in vec2 v_texcoord1;\n";
        s += "layout(location = 4) in vec2 v_texcoord2;\n";
        s += "layout(location = 5) in vec2 v_texcoord3;\n";
    } else if (two_coords) {
        s += "layout(location = 3) in vec2 v_texcoord1;\n";
    }
    if (program_count) {
        s += "layout(location = 6) in vec2 v_program_q;\n";
    }

    s += "layout(location = 0) out vec4 out_color;\n";
    if (two_coords) {
        // Cubic B-spline in four bilinear taps; see the Metal shader.
        s += "vec4 sample_smooth(sampler2D t, vec2 uv) {\n";
        s += "    vec2 size = vec2(textureSize(t, 0));\n";
        s += "    vec2 p = uv * size - 0.5;\n";
        s += "    vec2 f = fract(p);\n";
        s += "    p -= f;\n";
        s += "    vec2 f2 = f * f;\n";
        s += "    vec2 f3 = f2 * f;\n";
        s += "    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;\n";
        s += "    vec2 w3 = f3 / 6.0;\n";
        s += "    vec2 g0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0 + w1;\n";
        s += "    vec2 g1 = 1.0 - g0;\n";
        s += "    vec2 h0 = (p - 0.5 + w1 / g0) / size;\n";
        s += "    vec2 h1 = (p + 1.5 + w3 / g1) / size;\n";
        s += "    return g0.y * (g0.x * textureLod(t, h0, 0.0) + g1.x * textureLod(t, vec2(h1.x, h0.y), 0.0)) +\n";
        s += "           g1.y * (g0.x * textureLod(t, vec2(h0.x, h1.y), 0.0) + g1.x * textureLod(t, h1, 0.0));\n";
        s += "}\n";
    }
    s += "void main() {\n";
    s += "    vec2 tc = v_texcoord;\n";
    s += "    vec2 refl_tc = v_reflection_coord;\n";
    if (program_count) {
        s += "    tc /= v_program_q.x;\n";
        s += "    if (u.reflection_flags.z > 0.5) refl_tc /= v_program_q.y;\n";
    }
    s += "    vec4 shaded = v_color;\n";
    s += "    if (u.reflection_flags.x > 0.5) {\n";
    s += "        vec4 base = texture(guest_texture, tc);\n";
    s += "        base.a *= u.reflection_diffuse.a;\n";
    s += "        vec4 env = texture(alpha_mask, refl_tc);\n";
    s += "        vec4 diffuse = u.reflection_diffuse;\n";
    s += "        if (u.directional_flags.x > 0.5) diffuse.rgb = v_color.rgb;\n";
    s += "        shaded = mix(base, env, env.a) * diffuse;\n";
    s += "    } else\n";
    if (four_coords) {
        s += "    if (u.texture_flags.z > 0.5) {\n";
        s += "        vec4 t0 = texture(guest_texture, tc * u.texture_flags.xy);\n";
        s += "        vec4 t1 = texture(guest_texture, v_texcoord1 * u.texture_flags.xy);\n";
        s += "        vec4 t2 = texture(guest_texture, v_texcoord2 * u.texture_flags.xy);\n";
        s += "        vec4 t3 = texture(guest_texture, v_texcoord3 * u.texture_flags.xy);\n";
        s += "        shaded = 0.5 * (clamp((128.0 / 255.0) * (t0 + t1), 0.0, 1.0) + clamp((128.0 / 255.0) * (t2 + t3), 0.0, 1.0));\n";
        s += "    } else\n";
    } else if (two_coords) {
        s += "    if (u.texture_flags.w > 0.5) {\n";
        s += "        vec2 uv1 = v_texcoord1 * u.texture_flags.xy;\n";
        s += "        vec3 rgb = (u.texture_flags.w > 1.5 ? sample_smooth(guest_texture, uv1)\n";
        s += "            : texture(guest_texture, uv1)).rgb;\n";
        s += "        float alpha = texture(alpha_mask, tc * u.lighting_flags.zw).a;\n";
        s += "        shaded = v_color * vec4(rgb, alpha);\n";
        s += "    } else\n";
    }
    s += "    if (u.blend_flags.y == 1.0) {\n";
    s += "        shaded = u.texture_factor;\n";
    s += "    } else if (u.draw_flags.x > 0.5) {\n";
    s += "        vec4 sampled = texture(guest_texture, tc * u.texture_flags.xy);\n";
    s += "        if (u.lighting_flags.y > 0.5) sampled.rgb = vec3(1.0);\n";
    s += has_diffuse ? "        shaded *= sampled;\n" : "        shaded = sampled;\n";
    s += "        if (u.directional_flags.x > 0.5) shaded.rgb *= v_color.rgb;\n";
    s += "        if (u.lighting_flags.x > 0.5) shaded.rgb = vec3(0.0);\n";
    s += "        shaded.a = (u.blend_flags.w > 0.5) ? u.blend_flags.z : (shaded.a * u.blend_flags.z);\n";
    s += "        if (u.blend_flags.y > 1.5) shaded *= u.texture_factor;\n";
    s += "    }\n";
    s += "    if (u.reflection_flags.z > 0.5) {\n";
    s += "        shaded.a *= texture(alpha_mask, refl_tc, u.reflection_flags.w).a;\n";
    s += "    }\n";
    s += "    if (u.draw_flags.y > 0.5) {\n";
    s += "        float alpha = round(clamp(shaded.a, 0.0, 1.0) * 255.0);\n";
    s += "        float ref = u.draw_flags.w;\n";
    s += "        int func = int(u.draw_flags.z);\n";
    s += "        bool alpha_pass = (func == 7) ||\n";
    s += "            (func == 1 && alpha < ref) ||\n";
    s += "            (func == 2 && alpha == ref) ||\n";
    s += "            (func == 3 && alpha <= ref) ||\n";
    s += "            (func == 4 && alpha > ref) ||\n";
    s += "            (func == 5 && alpha != ref) ||\n";
    s += "            (func == 6 && alpha >= ref);\n";
    s += "        if (!alpha_pass) discard;\n";
    s += "    }\n";
    s += "    out_color = shaded;\n";
    s += "}\n";

    return s;
}
