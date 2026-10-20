#include "d3d_draw_model.h"
#include "runtime.h"

#include <stdio.h>

/* The DrawVerticesUP adapter delegates to the guest's generated driver body.
   The unit test links the adapter for its lookup entry point only, so this
   stub stands in for that body and simply retires the call. */
void sub_001E7750(void)
{
    recomp_runtime.registers.esp += 20u;
}

static int expect_u32(const char *field, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(
        stderr,
        "D3D draw: %s was %u, expected %u\n",
        field,
        (unsigned)actual,
        (unsigned)expected);
    return 0;
}

int recomp_d3d_draw_model_test(void)
{
    RecompD3dDrawState state;
    RecompD3dDrawResult result;
    int passed = 1;

    recomp_d3d_draw_reset(&state);
    passed &= expect_u32("reset stride", state.stream0.stride, 0u);

    /* The observed first draw: an 11-triangle fan over 13 indices, with a
       32-byte vertex bound from FVF 0x112 (XYZ | NORMAL | TEX1). */
    state.stream0.vertex_data = 0x822ca8b8u;
    state.stream0.stride = 0x20u;
    state.fvf = 0x112u;

    passed &= expect_u32("fvf 0x112 stride", recomp_d3d_fvf_stride(0x112u), 32u);
    passed &= expect_u32("fvf 0x0112 xyz only", recomp_d3d_fvf_stride(0x002u), 12u);
    passed &= expect_u32(
        "fvf diffuse", recomp_d3d_fvf_stride(0x042u), 16u);
    passed &= expect_u32("fvf xyzrhw stride", recomp_d3d_fvf_stride(0x004u), 16u);
    passed &= expect_u32(
        "fvf 0x342 nominal stride", recomp_d3d_fvf_stride(0x342u), 40u);
    passed &= expect_u32(
        "fvf 0x342 retail stream",
        recomp_d3d_fvf_for_stream(0x342u, 32u), 0x242u);
    passed &= expect_u32(
        "fvf 0x342 nominal stream unchanged",
        recomp_d3d_fvf_for_stream(0x342u, 40u), 0x342u);
    passed &= expect_u32(
        "fvf 0x312 retail stream",
        recomp_d3d_fvf_for_stream(0x312u, 40u), 0x212u);
    passed &= expect_u32(
        "fvf 0x312 nominal stream unchanged",
        recomp_d3d_fvf_for_stream(0x312u, 48u), 0x312u);
    passed &= expect_u32(
        "fvf unrelated stream unchanged",
        recomp_d3d_fvf_for_stream(0x112u, 32u), 0x112u);
    passed &= expect_u32(
        "fvf 0x316 swimsuit stream",
        recomp_d3d_fvf_for_stream(0x316u, 44u), 0x216u);
    passed &= expect_u32(
        "fvf 0x31a swimsuit stream",
        recomp_d3d_fvf_for_stream(0x31au, 52u), 0x21au);
    passed &= expect_u32(
        "fvf too short for any texcoords still exceeds stride",
        recomp_d3d_fvf_stride(recomp_d3d_fvf_for_stream(0x112u, 20u)) > 20u, 1u);

    /* Two unweighted FVFs this title draws with. 0x142 carries a diffuse color
       where 0x112 carries a normal, so their texcoords sit at different
       offsets: a consumer using one fixed layout for both reads the wrong
       bytes for whichever it was not built from. */
    {
        RecompD3dVertexLayout layout;

        passed &= expect_u32(
            "layout 0x112 decoded",
            recomp_d3d_fvf_layout(0x112u, &layout) ? 1u : 0u,
            1u);
        passed &= expect_u32("layout 0x112 stride", layout.stride, 32u);
        passed &= expect_u32("layout 0x112 position", layout.position_offset, 0u);
        passed &= expect_u32("layout 0x112 pretransformed", layout.pretransformed, 0u);
        passed &= expect_u32("layout 0x112 weights", layout.blend_weight_count, 0u);
        passed &= expect_u32("layout 0x112 normal", layout.normal_offset, 12u);
        passed &= expect_u32(
            "layout 0x112 diffuse",
            layout.diffuse_offset,
            RECOMP_D3D_FVF_ABSENT);
        passed &= expect_u32("layout 0x112 texcoord", layout.texcoord_offset, 24u);
        passed &= expect_u32("layout 0x112 texcoords", layout.texcoord_count, 1u);

        passed &= expect_u32(
            "layout 0x142 decoded",
            recomp_d3d_fvf_layout(0x142u, &layout) ? 1u : 0u,
            1u);
        passed &= expect_u32("layout 0x142 stride", layout.stride, 24u);
        passed &= expect_u32("layout 0x142 weights", layout.blend_weight_count, 0u);
        passed &= expect_u32(
            "layout 0x142 normal",
            layout.normal_offset,
            RECOMP_D3D_FVF_ABSENT);
        passed &= expect_u32("layout 0x142 diffuse", layout.diffuse_offset, 12u);
        passed &= expect_u32("layout 0x142 texcoord", layout.texcoord_offset, 16u);

        /* Stride agrees with the standalone size helper for every FVF. */
        passed &= expect_u32(
            "layout 0x142 stride matches",
            layout.stride,
            recomp_d3d_fvf_stride(0x142u));

        passed &= expect_u32(
            "layout glyph decoded",
            recomp_d3d_fvf_layout(0x144u, &layout) ? 1u : 0u,
            1u);
        passed &= expect_u32("layout glyph stride", layout.stride, 28u);
        passed &= expect_u32("layout glyph stride matches", layout.stride,
            recomp_d3d_fvf_stride(0x144u));
        passed &= expect_u32("layout glyph pretransformed", layout.pretransformed, 1u);
        passed &= expect_u32("layout glyph position", layout.position_offset, 0u);
        passed &= expect_u32("layout glyph weights", layout.blend_weight_count, 0u);
        passed &= expect_u32("layout glyph normal", layout.normal_offset, RECOMP_D3D_FVF_ABSENT);
        passed &= expect_u32("layout glyph diffuse", layout.diffuse_offset, 16u);
        passed &= expect_u32("layout glyph texcoord", layout.texcoord_offset, 20u);
        passed &= expect_u32("layout glyph texcoord count", layout.texcoord_count, 1u);
    }

    for (uint32_t weights = 1u; weights <= 3u; ++weights) {
        uint32_t fvf = 0x110u | (4u + weights * 2u);
        RecompD3dVertexLayout layout = {0};

        passed &= expect_u32("weighted stride", recomp_d3d_fvf_stride(fvf),
            32u + weights * 4u);
        passed &= expect_u32("weighted layout decoded",
            recomp_d3d_fvf_layout(fvf, &layout) ? 1u : 0u, 1u);
        passed &= expect_u32("weighted layout count", layout.blend_weight_count, weights);
        passed &= expect_u32("weighted layout position", layout.position_offset, 0u);
        passed &= expect_u32("weighted layout pretransformed", layout.pretransformed, 0u);
        passed &= expect_u32("weighted layout normal", layout.normal_offset, 12u + weights * 4u);
        passed &= expect_u32("weighted layout texcoord", layout.texcoord_offset, 24u + weights * 4u);
        passed &= expect_u32("weighted layout stride", layout.stride, 32u + weights * 4u);
    }
    {
        const uint32_t invalid[] = {
            0x114u, 0x11cu, 0x11eu,
            0x1116u, 0x1118u, 0x111au,
            0x8116u, 0x8118u, 0x811au,
        };

        for (uint32_t i = 0u; i < sizeof invalid / sizeof invalid[0]; ++i) {
            RecompD3dVertexLayout layout = {0};

            layout.stride = 123u;
            passed &= expect_u32("unsupported weighted stride",
                recomp_d3d_fvf_stride(invalid[i]), 0u);
            passed &= expect_u32("unsupported weighted layout",
                recomp_d3d_fvf_layout(invalid[i], &layout) ? 1u : 0u, 0u);
            passed &= expect_u32("rejected layout untouched", layout.stride, 123u);
        }
    }

    passed &= expect_u32(
        "fan triangles",
        recomp_d3d_draw_triangle_count(RECOMP_D3D_PT_TRIANGLEFAN, 13u),
        11u);
    passed &= expect_u32(
        "list triangles",
        recomp_d3d_draw_triangle_count(RECOMP_D3D_PT_TRIANGLELIST, 12u),
        4u);
    passed &= expect_u32(
        "strip triangles",
        recomp_d3d_draw_triangle_count(RECOMP_D3D_PT_TRIANGLESTRIP, 5u),
        3u);
    passed &= expect_u32(
        "degenerate fan",
        recomp_d3d_draw_triangle_count(RECOMP_D3D_PT_TRIANGLEFAN, 2u),
        0u);

    result = recomp_d3d_draw_indexed(
        &state, RECOMP_D3D_PT_TRIANGLEFAN, 13u, 0x822b1fc6u);
    passed &= expect_u32("fan accepted", result.error, RECOMP_D3D_DRAW_OK);
    passed &= expect_u32("plan triangles", result.plan.triangle_count, 11u);
    passed &= expect_u32("plan index bytes", result.plan.index_bytes, 26u);
    passed &= expect_u32("plan stride", result.plan.vertex_stride, 0x20u);
    passed &= expect_u32("plan fvf", result.plan.fvf, 0x112u);

    passed &= expect_u32(
        "vertex span",
        recomp_d3d_draw_vertex_bytes(0x20u, 12u),
        13u * 0x20u);
    passed &= expect_u32(
        "zero stride span", recomp_d3d_draw_vertex_bytes(0u, 12u), 0u);

    result = recomp_d3d_draw_indexed(
        &state, RECOMP_D3D_PT_TRIANGLEFAN, 13u, 0u);
    passed &= expect_u32(
        "null indices rejected",
        result.error,
        RECOMP_D3D_DRAW_INVALID_ARGUMENT);

    result = recomp_d3d_draw_indexed(&state, 1u, 13u, 0x822b1fc6u);
    passed &= expect_u32(
        "point list rejected",
        result.error,
        RECOMP_D3D_DRAW_UNSUPPORTED_PRIMITIVE);

    state.stream0.vertex_data = 0u;
    result = recomp_d3d_draw_indexed(
        &state, RECOMP_D3D_PT_TRIANGLEFAN, 13u, 0x822b1fc6u);
    passed &= expect_u32(
        "unbound stream rejected", result.error, RECOMP_D3D_DRAW_NO_STREAM);

    if (passed) {
        printf("D3D draw model: ok\n");
    }
    return passed;
}
