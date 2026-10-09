#include "d3d_render_state_adapter.h"
#include "d3d_render_state_model.h"
#include "program_manual.h"
#include "runtime.h"

#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
    TEST_STATIC_BASE = 0x001f0000u,
    TEST_STATIC_SIZE = 0x00007000u,
    TEST_CALL_BASE = 0x28000000u,
    TEST_CALL_SIZE = 0x00001000u,
    TEST_ENTRY_ESP = TEST_CALL_BASE + 0x200u,
    TEST_DEVICE = 0x001f3120u,
    TEST_DEVICE_GLOBAL = 0x001f2978u,
    TEST_DIRTY_MASK = 0x001f2984u,
    TEST_EDGE_ANTIALIAS_SHADOW = 0x001f2de4u,
    TEST_SHADOW = 0x001f2dc0u,
    TEST_TEXTURE_FACTOR_SHADOW = 0x001f2dd8u,
    TEST_FILL_MODE_SHADOW = 0x001f2db4u,
    TEST_Z_ENABLE_SHADOW = 0x001f2dc4u,
    TEST_CULL_SHADOW = 0x001f2dd4u,
    TEST_STENCIL_ENABLE_SHADOW = 0x001f2dc8u,
    TEST_STENCIL_FAIL_SHADOW = 0x001f2dccu,
    TEST_MULTISAMPLE_ANTIALIAS_SHADOW = 0x001f2de8u,
    TEST_DIRTY_BIT = 0x00000200u,
};

static int expect_u32(const char *field, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(
        stderr,
        "D3D NormalizeNormals: %s was 0x%08x, expected 0x%08x\n",
        field,
        actual,
        expected);
    return 0;
}

static void prepare_call(
    uint8_t *call_memory,
    uint32_t value,
    uint32_t return_address)
{
    uint32_t *stack = (uint32_t *)(void *)(
        call_memory + TEST_ENTRY_ESP - TEST_CALL_BASE);

    stack[0] = return_address;
    stack[1] = value;
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    recomp_runtime.registers.eax = 0xa5a5a5a5u;
}

/* The depth and alpha-test values below are the ones this title actually
   writes, taken from a full-run census of the simple render states, so the
   test fails if the decode of a state the game relies on ever drifts. */
static int depth_state_test(void)
{
    RecompD3dRenderStateModel model;
    RecompD3dDepthState state;
    int passed = 1;

    /* An untouched model must report the Xbox D3D8 defaults, not the host's:
       Direct3D 11 defaults depth compare to LESS, which is not the same. */
    recomp_d3d_render_state_reset(&model);
    recomp_d3d_depth_state(&model, &state);
    passed &= expect_u32(
        "default depth func", state.depth_func,
        RECOMP_D3D_COMPARE_LESS_EQUAL);
    passed &= expect_u32(
        "default depth write", state.depth_write_enable ? 1u : 0u, 1u);
    passed &= expect_u32(
        "default alpha test", state.alpha_test_enable ? 1u : 0u, 0u);

    /* ZENABLE is a separate entry point, so depth testing follows it. */
    passed &= expect_u32(
        "default depth test", state.depth_test_enable ? 1u : 0u, 0u);
    recomp_d3d_set_z_enable(&model, 1u);
    recomp_d3d_depth_state(&model, &state);
    passed &= expect_u32(
        "enabled depth test", state.depth_test_enable ? 1u : 0u, 1u);

    /* Observed live: DEPTH_MASK 0x035c = 1, DEPTH_FUNC 0x0354 absent,
       ALPHA_TEST 0x0300 = 0, ALPHA_FUNC 0x033c = 0x204 (GREATER),
       ALPHA_REF 0x0340 = 1. */
    recomp_d3d_set_simple_render_state(&model, 0x0004035cu, 0u);
    recomp_d3d_set_simple_render_state(&model, 0x00040354u, 0x00000204u);
    recomp_d3d_set_simple_render_state(&model, 0x00040300u, 1u);
    recomp_d3d_set_simple_render_state(&model, 0x0004033cu, 0x00000204u);
    recomp_d3d_set_simple_render_state(&model, 0x00040340u, 1u);
    recomp_d3d_depth_state(&model, &state);
    passed &= expect_u32(
        "depth write off", state.depth_write_enable ? 1u : 0u, 0u);
    passed &= expect_u32(
        "depth func greater", state.depth_func, RECOMP_D3D_COMPARE_GREATER);
    passed &= expect_u32(
        "alpha test on", state.alpha_test_enable ? 1u : 0u, 1u);
    passed &= expect_u32(
        "alpha func greater", state.alpha_func, RECOMP_D3D_COMPARE_GREATER);
    passed &= expect_u32("alpha ref", state.alpha_ref, 1u);

    /* Every GL comparison enum must map, and nothing outside the range may. */
    {
        RecompD3dCompareFunc func = RECOMP_D3D_COMPARE_NEVER;
        if (!recomp_d3d_compare_func_from_nv(0x0207u, &func) ||
            func != RECOMP_D3D_COMPARE_ALWAYS ||
            !recomp_d3d_compare_func_from_nv(0x0203u, &func) ||
            func != RECOMP_D3D_COMPARE_LESS_EQUAL ||
            recomp_d3d_compare_func_from_nv(0x0208u, &func) ||
            recomp_d3d_compare_func_from_nv(0x01ffu, &func)) {
            fprintf(stderr, "D3D depth state: compare decode wrong\n");
            passed = 0;
        }
    }
    return passed;
}

static int stencil_state_test(void)
{
    static const struct {
        uint32_t raw;
        RecompD3dStencilOp expected;
    } operations[] = {
        {0x1e00u, RECOMP_D3D_STENCIL_KEEP},
        {0x0000u, RECOMP_D3D_STENCIL_ZERO},
        {0x1e01u, RECOMP_D3D_STENCIL_REPLACE},
        {0x1e02u, RECOMP_D3D_STENCIL_INCRSAT},
        {0x1e03u, RECOMP_D3D_STENCIL_DECRSAT},
        {0x150au, RECOMP_D3D_STENCIL_INVERT},
        {0x8507u, RECOMP_D3D_STENCIL_INCRWRAP},
        {0x8508u, RECOMP_D3D_STENCIL_DECRWRAP},
    };
    RecompD3dRenderStateModel model;
    RecompD3dDepthState state;
    int passed = 1;

    recomp_d3d_render_state_reset(&model);
    for (uint32_t null_model = 0u; null_model < 2u; ++null_model) {
        memset(&state, 0xa5, sizeof state);
        recomp_d3d_depth_state(null_model ? NULL : &model, &state);
        passed &= expect_u32("default stencil enable", state.stencil_enable, 0u);
        passed &= expect_u32(
            "default stencil func", state.stencil_func, RECOMP_D3D_COMPARE_ALWAYS);
        passed &= expect_u32("default stencil ref", state.stencil_ref, 0u);
        passed &= expect_u32("default stencil read mask", state.stencil_read_mask, 0xffu);
        passed &= expect_u32("default stencil write mask", state.stencil_write_mask, 0xffu);
        passed &= expect_u32("default stencil fail", state.stencil_fail, RECOMP_D3D_STENCIL_KEEP);
        passed &= expect_u32("default stencil zfail", state.stencil_zfail, RECOMP_D3D_STENCIL_KEEP);
        passed &= expect_u32("default stencil pass", state.stencil_pass, RECOMP_D3D_STENCIL_KEEP);
    }

    /* The paired shadow draws first invert stencil, then test equality and
       clear it; their reference and both masks remain 1/3/3. */
    recomp_d3d_set_stencil_enable(&model, 1u);
    recomp_d3d_set_simple_render_state(&model, 0x00040364u, 0x0203u);
    recomp_d3d_set_simple_render_state(&model, 0x00040368u, 1u);
    recomp_d3d_set_simple_render_state(&model, 0x0004036cu, 3u);
    recomp_d3d_set_simple_render_state(&model, 0x00040360u, 3u);
    recomp_d3d_set_simple_render_state(&model, 0x00040370u, 0x1e00u);
    recomp_d3d_set_simple_render_state(&model, 0x00040374u, 0x1e00u);
    recomp_d3d_set_simple_render_state(&model, 0x00040378u, 0x150au);
    recomp_d3d_depth_state(&model, &state);
    passed &= expect_u32("shadow stencil enable", state.stencil_enable, 1u);
    passed &= expect_u32("shadow stencil func", state.stencil_func, RECOMP_D3D_COMPARE_LESS_EQUAL);
    passed &= expect_u32("shadow stencil ref", state.stencil_ref, 1u);
    passed &= expect_u32("shadow stencil read mask", state.stencil_read_mask, 3u);
    passed &= expect_u32("shadow stencil write mask", state.stencil_write_mask, 3u);
    passed &= expect_u32("shadow stencil fail", state.stencil_fail, RECOMP_D3D_STENCIL_KEEP);
    passed &= expect_u32("shadow stencil zfail", state.stencil_zfail, RECOMP_D3D_STENCIL_KEEP);
    passed &= expect_u32("shadow stencil pass", state.stencil_pass, RECOMP_D3D_STENCIL_INVERT);
    recomp_d3d_set_simple_render_state(&model, 0x00040364u, 0x0202u);
    recomp_d3d_set_simple_render_state(&model, 0x00040378u, 0u);
    recomp_d3d_depth_state(&model, &state);
    passed &= expect_u32("second shadow stencil func", state.stencil_func, RECOMP_D3D_COMPARE_EQUAL);
    passed &= expect_u32("second shadow stencil pass", state.stencil_pass, RECOMP_D3D_STENCIL_ZERO);

    for (size_t i = 0u; i < sizeof operations / sizeof operations[0]; ++i) {
        size_t next = (i + 1u) % (sizeof operations / sizeof operations[0]);
        size_t last = (i + 2u) % (sizeof operations / sizeof operations[0]);
        recomp_d3d_set_simple_render_state(&model, 0x00040370u, operations[i].raw);
        recomp_d3d_set_simple_render_state(&model, 0x00040374u, operations[next].raw);
        recomp_d3d_set_simple_render_state(&model, 0x00040378u, operations[last].raw);
        recomp_d3d_depth_state(&model, &state);
        passed &= expect_u32("stencil fail decode", state.stencil_fail, operations[i].expected);
        passed &= expect_u32("stencil zfail decode", state.stencil_zfail, operations[next].expected);
        passed &= expect_u32("stencil pass decode", state.stencil_pass, operations[last].expected);
    }

    recomp_d3d_set_simple_render_state(&model, 0x00040368u, 0x123401u);
    recomp_d3d_set_simple_render_state(&model, 0x0004036cu, 0x123403u);
    recomp_d3d_set_simple_render_state(&model, 0x00040360u, 0xffffffu);
    recomp_d3d_depth_state(&model, &state);
    passed &= expect_u32("stencil ref low byte", state.stencil_ref, 1u);
    passed &= expect_u32("stencil read mask low byte", state.stencil_read_mask, 3u);
    passed &= expect_u32("stencil write mask low byte", state.stencil_write_mask, 0xffu);
    recomp_d3d_set_stencil_enable(&model, 0u);
    recomp_d3d_set_simple_render_state(&model, 0x00040368u, 0u);
    recomp_d3d_set_simple_render_state(&model, 0x0004036cu, 0u);
    recomp_d3d_set_simple_render_state(&model, 0x00040360u, 0u);
    recomp_d3d_depth_state(&model, &state);
    passed &= expect_u32("stencil disable", state.stencil_enable, 0u);
    passed &= expect_u32("zero stencil ref", state.stencil_ref, 0u);
    passed &= expect_u32("zero stencil read mask", state.stencil_read_mask, 0u);
    passed &= expect_u32("zero stencil write mask", state.stencil_write_mask, 0u);
    {
        RecompD3dStencilOp op = RECOMP_D3D_STENCIL_INVERT;
        passed &= expect_u32("invalid stencil operation", recomp_d3d_stencil_op_from_nv(0x1e04u, &op), 0u);
        passed &= expect_u32("invalid stencil output unchanged", op, RECOMP_D3D_STENCIL_INVERT);
        passed &= expect_u32("null stencil operation", recomp_d3d_stencil_op_from_nv(0x1e00u, NULL), 0u);
    }
    return passed;
}

static int texture_factor_selector_test(void)
{
    static const uint32_t cases[][5] = {
        {2u, 3u, 2u, 3u, 1u},
        {3u, 3u, 2u, 3u, 0u},
        {2u, 2u, 2u, 3u, 0u},
        {2u, 3u, 3u, 3u, 0u},
        {2u, 3u, 2u, 2u, 0u},
        {2u, 0x13u, 2u, 3u, 0u},
        {2u, 3u, 2u, 0x13u, 0u},
        {0u, 0u, 0u, 0u, 0u},
    };
    int passed = 1;

    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        passed &= expect_u32(
            "texture factor selector",
            recomp_d3d_texture_factor_selected(
                cases[i][0], cases[i][1], cases[i][2], cases[i][3]),
            cases[i][4]);
    }
    return passed;
}

static int texture_factor_modulate_selector_test(void)
{
    static const uint32_t cases[][8] = {
        {4u, 3u, 1u, 4u, 3u, 1u, 1u, 1u},
        {1u, 3u, 1u, 4u, 3u, 1u, 1u, 0u}, /* Disabled color stage. */
        {4u, 1u, 3u, 4u, 3u, 1u, 1u, 0u}, /* Different argument order. */
        {4u, 3u, 1u, 4u, 0x13u, 1u, 1u, 0u}, /* Complemented factor. */
        {4u, 3u, 1u, 4u, 3u, 1u, 4u, 0u}, /* Another active stage. */
    };
    int passed = expect_u32(
        "null factor modulation stage",
        recomp_d3d_texture_factor_modulate_selected(NULL, 1u), 0u);

    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        passed &= expect_u32(
            "texture factor modulation selector",
            recomp_d3d_texture_factor_modulate_selected(cases[i], cases[i][6]),
            cases[i][7]);
    }
    return passed;
}

static int zero_diffuse_rgb_test(void)
{
    static const struct {
        uint32_t ambient, active_light_head;
        float emissive[3];
        bool expected;
    } cases[] = {
        {0xff000000u, 0u, {0.0f, 0.0f, 0.0f}, true},
        {0u, 0u, {-0.0f, 0.0f, 0.0f}, true},
        {0xff000001u, 0u, {0.0f, 0.0f, 0.0f}, false},
        {0xff000000u, 0x001f5000u, {0.0f, 0.0f, 0.0f}, false},
        {0xff000000u, 0u, {0.25f, 0.0f, 0.0f}, false},
        {0xff000000u, 0u, {0.0f, 0.25f, 0.0f}, false},
        {0xff000000u, 0u, {0.0f, 0.0f, 0.25f}, false},
        {0xff000000u, 0u, {NAN, 0.0f, 0.0f}, false},
        {0xff000000u, 0u, {0.0f, INFINITY, 0.0f}, false},
    };
    int passed = expect_u32("zero diffuse missing emissive",
        recomp_d3d_diffuse_rgb_is_zero(0u, 0u, NULL), false);

    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        passed &= expect_u32("zero diffuse RGB",
            recomp_d3d_diffuse_rgb_is_zero(
                cases[i].ambient, cases[i].active_light_head, cases[i].emissive),
            cases[i].expected);
    }
    return passed;
}

static int texture_material_alpha_mode_test(void)
{
    static const uint32_t cases[][7] = {
        {2u, 2u, 1u, 4u, 2u, 0u, RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE},
        {4u, 2u, 0u, 2u, 0u, 1u, RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE},
        {4u, 2u, 0u, 4u, 2u, 0u, RECOMP_D3D_MATERIAL_ALPHA_NONE}, /* Mode 1. */
        {2u, 2u, 1u, 2u, 0u, 1u, RECOMP_D3D_MATERIAL_ALPHA_NONE}, /* Mode 2. */
        {2u, 3u, 1u, 2u, 3u, 1u, RECOMP_D3D_MATERIAL_ALPHA_NONE}, /* TFACTOR. */
        {2u, 2u, 1u, 4u, 2u, 0x10u, RECOMP_D3D_MATERIAL_ALPHA_NONE}, /* Complement. */
        {2u, 2u, 0u, 4u, 2u, 0u, RECOMP_D3D_MATERIAL_ALPHA_NONE}, /* Wrong color arg2. */
    };
    int passed = 1;

    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        passed &= expect_u32(
            "texture material alpha mode",
            recomp_d3d_texture_material_alpha_mode(
                cases[i][0], cases[i][1], cases[i][2],
                cases[i][3], cases[i][4], cases[i][5]),
            cases[i][6]);
    }
    return passed;
}

/* Blend values observed live: BLEND_ENABLE 0x0304 = 1, SRC 0x0344 = 0x302
   (SRC_ALPHA), DST 0x0348 = 0x303 (INV_SRC_ALPHA), EQUATION 0x0350 = 0x8006
   (ADD) - the standard translucency setup. */
static int blend_state_test(void)
{
    RecompD3dRenderStateModel model;
    RecompD3dBlendState state;
    int passed = 1;

    recomp_d3d_render_state_reset(&model);
    recomp_d3d_blend_state(&model, &state);
    passed &= expect_u32(
        "default blend enable", state.blend_enable ? 1u : 0u, 0u);
    passed &= expect_u32(
        "absent color write mask", state.color_write_mask, 0x0fu);
    state.color_write_mask = 0u;
    recomp_d3d_blend_state(NULL, &state);
    passed &= expect_u32(
        "null model color write mask", state.color_write_mask, 0x0fu);

    recomp_d3d_set_simple_render_state(&model, 0x00040304u, 1u);
    recomp_d3d_set_simple_render_state(&model, 0x00040344u, 0x00000302u);
    recomp_d3d_set_simple_render_state(&model, 0x00040348u, 0x00000303u);
    recomp_d3d_set_simple_render_state(&model, 0x00040350u, 0x00008006u);
    recomp_d3d_blend_state(&model, &state);
    passed &= expect_u32("blend enable", state.blend_enable ? 1u : 0u, 1u);
    passed &= expect_u32(
        "blend src", state.src_factor, RECOMP_D3D_BLEND_SRC_ALPHA);
    passed &= expect_u32(
        "blend dst", state.dst_factor, RECOMP_D3D_BLEND_INV_SRC_ALPHA);
    passed &= expect_u32("blend op", state.op, RECOMP_D3D_BLEND_OP_ADD);
    recomp_d3d_set_simple_render_state(&model, 0x00040344u, 0x8001u);
    recomp_d3d_set_simple_render_state(&model, 0x00040348u, 0x8002u);
    recomp_d3d_set_simple_render_state(&model, 0x0004034cu, 0x40802010u);
    recomp_d3d_blend_state(&model, &state);
    passed &= expect_u32("constant blend source", state.src_factor, RECOMP_D3D_BLEND_CONSTANT_COLOR);
    passed &= expect_u32("inverse constant destination", state.dst_factor, RECOMP_D3D_BLEND_INV_CONSTANT_COLOR);
    passed &= expect_u32("ARGB blend constant", state.constant_color, 0x40802010u);

    {
        static const struct {
            const char *name;
            uint32_t raw;
            uint8_t expected;
        } cases[] = {
            {"zero color write mask", 0x00000000u, 0x00u},
            {"red color write mask", 0x00010000u, 0x01u},
            {"green color write mask", 0x00000100u, 0x02u},
            {"blue color write mask", 0x00000001u, 0x04u},
            {"alpha color write mask", 0x01000000u, 0x08u},
            {"RGB color write mask", 0x00010101u, 0x07u},
            {"RGBA color write mask", 0x01010101u, 0x0fu},
            {"reserved color write bits", 0xfefefefeu, 0x00u},
            {"all color write bits", 0xffffffffu, 0x0fu},
        };
        for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
            recomp_d3d_set_simple_render_state(
                &model, 0x00040358u, cases[i].raw);
            recomp_d3d_blend_state(&model, &state);
            passed &= expect_u32(
                cases[i].name, state.color_write_mask, cases[i].expected);
        }
    }

    {
        RecompD3dBlendFactor factor = RECOMP_D3D_BLEND_ZERO;
        RecompD3dBlendOp op = RECOMP_D3D_BLEND_OP_ADD;

        /* ZERO and ONE live outside the 0x03xx block. */
        if (!recomp_d3d_blend_factor_from_nv(0x0000u, &factor) ||
            factor != RECOMP_D3D_BLEND_ZERO ||
            !recomp_d3d_blend_factor_from_nv(0x0001u, &factor) ||
            factor != RECOMP_D3D_BLEND_ONE ||
            recomp_d3d_blend_factor_from_nv(0x0002u, &factor) ||
            !recomp_d3d_blend_factor_from_nv(0x8001u, &factor) ||
            factor != RECOMP_D3D_BLEND_CONSTANT_COLOR ||
            !recomp_d3d_blend_factor_from_nv(0x8002u, &factor) ||
            factor != RECOMP_D3D_BLEND_INV_CONSTANT_COLOR) {
            fprintf(stderr, "D3D blend state: factor decode wrong\n");
            passed = 0;
        }
        /* 0x8009 is not an equation, and the signed NV extensions have no
           host equivalent, so both must be refused. */
        if (!recomp_d3d_blend_op_from_nv(0x800bu, &op) ||
            op != RECOMP_D3D_BLEND_OP_REVERSE_SUBTRACT ||
            recomp_d3d_blend_op_from_nv(0x8009u, &op) ||
            recomp_d3d_blend_op_from_nv(0xf005u, &op)) {
            fprintf(stderr, "D3D blend state: equation decode wrong\n");
            passed = 0;
        }
    }
    return passed;
}

int recomp_d3d_render_state_model_test(void)
{
    static uint8_t static_memory[TEST_STATIC_SIZE];
    static uint8_t call_memory[TEST_CALL_SIZE];
    const RecompMemoryRegion regions[] = {
        {
            .address = TEST_STATIC_BASE,
            .size = sizeof static_memory,
            .data = static_memory,
        },
        {
            .address = TEST_CALL_BASE,
            .size = sizeof call_memory,
            .data = call_memory,
        },
    };
    const RecompD3dRenderStateModel *model;
    RecompFunction adapter;
    int passed = 1;

    /* Plain model: both documented BOOLs are accepted, anything else is
       rejected without touching the model. */
    {
        RecompD3dRenderStateModel plain;
        uint32_t simple_value = 0xa5a5a5a5u;

        recomp_d3d_render_state_reset(&plain);
        passed &= expect_u32("reset value", plain.normalize_normals, 0u);
        passed &= expect_u32(
            "reset update count", plain.normalize_normals_update_count, 0u);

        if (!recomp_d3d_set_normalize_normals(&plain, 0u)) {
            fprintf(stderr, "D3D NormalizeNormals: model rejected 0\n");
            passed = 0;
        }
        passed &= expect_u32("value after 0", plain.normalize_normals, 0u);
        passed &= expect_u32(
            "count after 0", plain.normalize_normals_update_count, 1u);

        if (!recomp_d3d_set_normalize_normals(&plain, 1u)) {
            fprintf(stderr, "D3D NormalizeNormals: model rejected 1\n");
            passed = 0;
        }
        passed &= expect_u32("value after 1", plain.normalize_normals, 1u);
        passed &= expect_u32(
            "count after 1", plain.normalize_normals_update_count, 2u);

        if (recomp_d3d_set_normalize_normals(&plain, 2u)) {
            fprintf(stderr, "D3D NormalizeNormals: model accepted 2\n");
            passed = 0;
        }
        if (recomp_d3d_set_normalize_normals(&plain, 0xffffffffu)) {
            fprintf(
                stderr, "D3D NormalizeNormals: model accepted 0xffffffff\n");
            passed = 0;
        }
        if (recomp_d3d_set_normalize_normals(NULL, 1u)) {
            fprintf(
                stderr, "D3D NormalizeNormals: model accepted null model\n");
            passed = 0;
        }
        passed &= expect_u32(
            "value after rejections", plain.normalize_normals, 1u);
        passed &= expect_u32(
            "count after rejections",
            plain.normalize_normals_update_count,
            2u);
        if (!recomp_d3d_set_texture_factor(&plain, 0u) ||
            !recomp_d3d_set_texture_factor(&plain, 0xffffffffu)) {
            fprintf(stderr, "D3D texture factor: model rejected D3DCOLOR\n");
            passed = 0;
        }
        passed &= expect_u32(
            "texture factor value", plain.texture_factor, 0xffffffffu);
        passed &= expect_u32(
            "texture factor count", plain.texture_factor_update_count, 2u);
        if (recomp_d3d_set_texture_factor(NULL, 0u)) {
            fprintf(stderr, "D3D texture factor: accepted null model\n");
            passed = 0;
        }
        if (!recomp_d3d_set_cull_mode(&plain, 0u) ||
            !recomp_d3d_set_cull_mode(&plain, 0x900u) ||
            !recomp_d3d_set_cull_mode(&plain, 0x901u)) {
            fprintf(stderr, "D3D cull mode: model rejected valid value\n");
            passed = 0;
        }
        passed &= expect_u32("cull value", plain.cull_mode, 0x901u);
        passed &= expect_u32("cull count", plain.cull_mode_update_count, 3u);
        if (recomp_d3d_set_cull_mode(&plain, 1u) ||
            recomp_d3d_set_cull_mode(&plain, 0x902u)) {
            fprintf(stderr, "D3D cull mode: invalid value accepted\n");
            passed = 0;
        }
        passed &= expect_u32(
            "cull count after rejections", plain.cull_mode_update_count, 3u);
        if (!recomp_d3d_set_multisample_antialias(&plain, 0u) ||
            !recomp_d3d_set_multisample_antialias(&plain, 1u)) {
            fprintf(stderr, "D3D multisample AA: model rejected BOOL\n");
            passed = 0;
        }
        passed &= expect_u32(
            "multisample AA value", plain.multisample_antialias, 1u);
        passed &= expect_u32(
            "multisample AA count",
            plain.multisample_antialias_update_count,
            2u);
        if (recomp_d3d_set_multisample_antialias(&plain, 2u)) {
            fprintf(stderr, "D3D multisample AA: model accepted 2\n");
            passed = 0;
        }
        if (!recomp_d3d_set_stencil_enable(&plain, 0u) ||
            !recomp_d3d_set_stencil_enable(&plain, 1u)) {
            fprintf(stderr, "D3D stencil enable: model rejected BOOL\n");
            passed = 0;
        }
        passed &= expect_u32(
            "stencil enable value", plain.stencil_enable, 1u);
        passed &= expect_u32(
            "stencil enable count", plain.stencil_enable_update_count, 2u);
        if (recomp_d3d_set_stencil_enable(&plain, 2u)) {
            fprintf(stderr, "D3D stencil enable: model accepted 2\n");
            passed = 0;
        }
        if (!recomp_d3d_set_z_enable(&plain, 0u) ||
            !recomp_d3d_set_z_enable(&plain, 1u) ||
            !recomp_d3d_set_z_enable(&plain, 2u)) {
            fprintf(stderr, "D3D Z enable: model rejected valid enum\n");
            passed = 0;
        }
        passed &= expect_u32("Z enable value", plain.z_enable, 2u);
        passed &= expect_u32(
            "Z enable count", plain.z_enable_update_count, 3u);
        if (recomp_d3d_set_z_enable(&plain, 3u)) {
            fprintf(stderr, "D3D Z enable: model accepted 3\n");
            passed = 0;
        }
        if (!recomp_d3d_set_fill_mode(&plain, 0x1b00u) ||
            !recomp_d3d_set_fill_mode(&plain, 0x1b01u) ||
            !recomp_d3d_set_fill_mode(&plain, 0x1b02u)) {
            fprintf(stderr, "D3D fill mode: model rejected valid enum\n");
            passed = 0;
        }
        passed &= expect_u32("fill mode value", plain.fill_mode, 0x1b02u);
        passed &= expect_u32(
            "fill mode count", plain.fill_mode_update_count, 3u);
        if (recomp_d3d_set_fill_mode(&plain, 0u)) {
            fprintf(stderr, "D3D fill mode: model accepted 0\n");
            passed = 0;
        }
        if (!recomp_d3d_set_edge_antialias(&plain, 0u) ||
            !recomp_d3d_set_edge_antialias(&plain, 1u)) {
            fprintf(stderr, "D3D edge AA: model rejected BOOL\n");
            passed = 0;
        }
        passed &= expect_u32(
            "edge AA value", plain.edge_antialias, 1u);
        passed &= expect_u32(
            "edge AA count", plain.edge_antialias_update_count, 2u);
        if (recomp_d3d_set_edge_antialias(&plain, 2u)) {
            fprintf(stderr, "D3D edge AA: model accepted 2\n");
            passed = 0;
        }

        if (!recomp_d3d_set_simple_render_state(
                &plain, 0x00040344u, 0x00000302u) ||
            !recomp_d3d_get_simple_render_state(
                &plain, 0x00040344u, &simple_value)) {
            fprintf(stderr, "D3D simple state: model rejected valid method\n");
            passed = 0;
        }
        passed &= expect_u32("simple value", simple_value, 0x00000302u);
        passed &= expect_u32("simple count", plain.simple_update_count, 1u);
        if (!recomp_d3d_set_simple_render_state(
                &plain, 0x00040344u, 0x00000303u) ||
            !recomp_d3d_get_simple_render_state(
                &plain, 0x00040344u, &simple_value)) {
            fprintf(stderr, "D3D simple state: model rejected overwrite\n");
            passed = 0;
        }
        passed &= expect_u32(
            "simple overwritten value", simple_value, 0x00000303u);
        passed &= expect_u32("simple overwrite count", plain.simple_update_count, 2u);
        if (!recomp_d3d_set_simple_render_state(
                &plain, 0x00040260u, 0x11223344u) ||
            !recomp_d3d_get_simple_render_state(
                &plain, 0x00040260u, &simple_value)) {
            fprintf(stderr, "D3D simple state: lower method rejected\n");
            passed = 0;
        }
        passed &= expect_u32(
            "lower method value", simple_value, 0x11223344u);
        if (!recomp_d3d_set_simple_render_state(
                &plain, 0x00040a60u, 0x55667788u) ||
            !recomp_d3d_get_simple_render_state(
                &plain, 0x00040a60u, &simple_value)) {
            fprintf(stderr, "D3D simple state: upper method rejected\n");
            passed = 0;
        }
        passed &= expect_u32(
            "upper method value", simple_value, 0x55667788u);
        passed &= expect_u32(
            "simple observed-domain count", plain.simple_update_count, 4u);
        if (recomp_d3d_get_simple_render_state(
                &plain, 0x00040348u, &simple_value) ||
            recomp_d3d_set_simple_render_state(
                &plain, 0x00040345u, 0u) ||
            recomp_d3d_set_simple_render_state(
                &plain, 0x00030344u, 0u) ||
            recomp_d3d_set_simple_render_state(
                &plain, 0x00042000u, 0u)) {
            fprintf(stderr, "D3D simple state: invalid method accepted\n");
            passed = 0;
        }
        passed &= expect_u32(
            "simple count after rejections", plain.simple_update_count, 4u);

        recomp_d3d_render_state_reset(NULL);
    }

    memset(static_memory, 0xa5, sizeof static_memory);
    memset(call_memory, 0, sizeof call_memory);
    recomp_runtime_init(regions, 2u, NULL, 0u, NULL, 0u);
    *recomp_memory_u32(TEST_DEVICE_GLOBAL) = TEST_DEVICE;
    *recomp_memory_u32(TEST_DIRTY_MASK) = 0x00004100u;
    *recomp_memory_u32(TEST_EDGE_ANTIALIAS_SHADOW) = 0xdeadbeefu;
    *recomp_memory_u32(TEST_SHADOW) = 0xdeadbeefu;
    *recomp_memory_u32(TEST_TEXTURE_FACTOR_SHADOW) = 0xdeadbeefu;
    *recomp_memory_u32(TEST_FILL_MODE_SHADOW) = 0xdeadbeefu;
    *recomp_memory_u32(TEST_Z_ENABLE_SHADOW) = 0xdeadbeefu;
    *recomp_memory_u32(TEST_CULL_SHADOW) = 0xdeadbeefu;
    *recomp_memory_u32(TEST_STENCIL_ENABLE_SHADOW) = 0xdeadbeefu;
    *recomp_memory_u32(TEST_MULTISAMPLE_ANTIALIAS_SHADOW) = 0xdeadbeefu;
    recomp_d3d_render_state_adapter_reset();
    model = recomp_d3d_render_state_adapter_model();

    adapter = recomp_lookup_manual(0x001e5200u);
    if (adapter == NULL) {
        fprintf(stderr, "D3D NormalizeNormals: manual lookup did not resolve\n");
        return 0;
    }
    if (adapter != recomp_d3d_set_normalize_normals_adapter) {
        fprintf(
            stderr, "D3D NormalizeNormals: lookup resolved to another body\n");
        passed = 0;
    }
    if (recomp_lookup_manual(0x001e51fcu) != NULL ||
        recomp_lookup_manual(0x001e5204u) != NULL) {
        fprintf(stderr, "D3D NormalizeNormals: lookup was not exact\n");
        passed = 0;
    }

    adapter = recomp_lookup_manual(0x001e4d80u);
    if (adapter != recomp_d3d_set_simple_render_state_adapter ||
        recomp_lookup_manual(0x001e4d7fu) != NULL ||
        recomp_lookup_manual(0x001e4d81u) != NULL) {
        fprintf(stderr, "D3D simple state: lookup was not exact\n");
        passed = 0;
    } else {
        uint32_t simple_value = 0u;

        *recomp_memory_u32(TEST_ENTRY_ESP) = 0x0010abcdu;
        recomp_runtime.registers.esp = TEST_ENTRY_ESP;
        recomp_runtime.registers.eax = 0xa5a5a5a5u;
        recomp_runtime.registers.ecx = 0x00040344u;
        recomp_runtime.registers.edx = 0x00000302u;
        adapter();
        if (!recomp_d3d_get_simple_render_state(
                model, 0x00040344u, &simple_value)) {
            fprintf(stderr, "D3D simple state: adapter state missing\n");
            passed = 0;
        }
        passed &= expect_u32(
            "simple adapter value", simple_value, 0x00000302u);
        passed &= expect_u32(
            "simple adapter count", model->simple_update_count, 1u);
        passed &= expect_u32(
            "simple ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 4u);
        passed &= expect_u32(
            "simple ECX", recomp_runtime.registers.ecx, 0x00040344u);
        passed &= expect_u32(
            "simple EDX", recomp_runtime.registers.edx, 0x00000302u);
        passed &= expect_u32(
            "simple dirty unchanged",
            *recomp_memory_u32(TEST_DIRTY_MASK),
            0x00004100u);
        passed &= expect_u32(
            "simple shadow unchanged",
            *recomp_memory_u32(TEST_SHADOW),
            0xdeadbeefu);
    }

    adapter = recomp_lookup_manual(0x001e5240u);
    if (adapter != recomp_d3d_set_texture_factor_adapter ||
        recomp_lookup_manual(0x001e523fu) != NULL ||
        recomp_lookup_manual(0x001e5241u) != NULL) {
        fprintf(stderr, "D3D texture factor: lookup was not exact\n");
        passed = 0;
    } else {
        prepare_call(call_memory, 0xff00aa55u, 0x0010abcdu);
        adapter();
        passed &= expect_u32(
            "texture factor adapter value",
            model->texture_factor,
            0xff00aa55u);
        passed &= expect_u32(
            "texture factor adapter count",
            model->texture_factor_update_count,
            1u);
        passed &= expect_u32(
            "texture factor shadow",
            *recomp_memory_u32(TEST_TEXTURE_FACTOR_SHADOW),
            0xff00aa55u);
        passed &= expect_u32(
            "texture factor ESP",
            recomp_runtime.registers.esp,
            TEST_ENTRY_ESP + 8u);
        passed &= expect_u32(
            "texture factor dirty unchanged",
            *recomp_memory_u32(TEST_DIRTY_MASK),
            0x00004100u);
    }

    adapter = recomp_lookup_manual(0x001e5150u);
    if (adapter != recomp_d3d_set_cull_mode_adapter ||
        recomp_lookup_manual(0x001e514fu) != NULL ||
        recomp_lookup_manual(0x001e5151u) != NULL) {
        fprintf(stderr, "D3D cull mode: lookup was not exact\n");
        passed = 0;
    } else {
        prepare_call(call_memory, 0x901u, 0x0010abcdu);
        adapter();
        passed &= expect_u32("cull adapter value", model->cull_mode, 0x901u);
        passed &= expect_u32(
            "cull adapter count", model->cull_mode_update_count, 1u);
        passed &= expect_u32(
            "cull shadow", *recomp_memory_u32(TEST_CULL_SHADOW), 0x901u);
        passed &= expect_u32(
            "cull ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 8u);
        passed &= expect_u32(
            "cull dirty unchanged",
            *recomp_memory_u32(TEST_DIRTY_MASK),
            0x00004100u);
    }

    adapter = recomp_lookup_manual(0x001e5200u);
    prepare_call(call_memory, 1u, 0x0010abcd);
    adapter();
    passed &= expect_u32("adapter value after 1", model->normalize_normals, 1u);
    passed &= expect_u32(
        "adapter count after 1", model->normalize_normals_update_count, 1u);
    passed &= expect_u32("shadow after 1", *recomp_memory_u32(TEST_SHADOW), 1u);
    passed &= expect_u32(
        "dirty mask after 1",
        *recomp_memory_u32(TEST_DIRTY_MASK),
        0x00004100u | TEST_DIRTY_BIT);
    passed &= expect_u32(
        "shadow following word",
        *recomp_memory_u32(TEST_SHADOW + 4u),
        0xdeadbeefu);
    passed &= expect_u32(
        "shadow preceding word",
        *recomp_memory_u32(TEST_SHADOW - 4u),
        0xa5a5a5a5u);
    passed &= expect_u32(
        "ESP after 1", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 8u);
    passed &= expect_u32(
        "void return EAX", recomp_runtime.registers.eax, 0xa5a5a5a5u);
    passed &= expect_u32(
        "argument preserved", *recomp_memory_u32(TEST_ENTRY_ESP + 4u), 1u);

    prepare_call(call_memory, 0u, 0x0010abcd);
    adapter();
    passed &= expect_u32("adapter value after 0", model->normalize_normals, 0u);
    passed &= expect_u32(
        "adapter count after 0", model->normalize_normals_update_count, 2u);
    passed &= expect_u32("shadow after 0", *recomp_memory_u32(TEST_SHADOW), 0u);
    passed &= expect_u32(
        "dirty mask idempotent",
        *recomp_memory_u32(TEST_DIRTY_MASK),
        0x00004100u | TEST_DIRTY_BIT);
    passed &= expect_u32(
        "ESP after 0", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 8u);

    adapter = recomp_lookup_manual(0x001e6510u);
    if (adapter != recomp_d3d_set_multisample_antialias_adapter ||
        recomp_lookup_manual(0x001e650fu) != NULL ||
        recomp_lookup_manual(0x001e6511u) != NULL) {
        fprintf(stderr, "D3D multisample AA: lookup was not exact\n");
        passed = 0;
    } else {
        prepare_call(call_memory, 0u, 0x0010abcdu);
        adapter();
        passed &= expect_u32(
            "multisample AA adapter value",
            model->multisample_antialias,
            0u);
        passed &= expect_u32(
            "multisample AA adapter count",
            model->multisample_antialias_update_count,
            1u);
        passed &= expect_u32(
            "multisample AA shadow",
            *recomp_memory_u32(TEST_MULTISAMPLE_ANTIALIAS_SHADOW),
            0u);
        passed &= expect_u32(
            "multisample AA ESP",
            recomp_runtime.registers.esp,
            TEST_ENTRY_ESP + 8u);
    }

    adapter = recomp_lookup_manual(0x001e6220u);
    if (adapter != recomp_d3d_set_stencil_enable_adapter ||
        recomp_lookup_manual(0x001e621fu) != NULL ||
        recomp_lookup_manual(0x001e6221u) != NULL) {
        fprintf(stderr, "D3D stencil enable: lookup was not exact\n");
        passed = 0;
    } else {
        prepare_call(call_memory, 0u, 0x0010abcdu);
        adapter();
        passed &= expect_u32(
            "stencil enable adapter value", model->stencil_enable, 0u);
        passed &= expect_u32(
            "stencil enable adapter count",
            model->stencil_enable_update_count,
            1u);
        passed &= expect_u32(
            "stencil enable shadow",
            *recomp_memory_u32(TEST_STENCIL_ENABLE_SHADOW),
            0u);
        passed &= expect_u32(
            "stencil enable ESP",
            recomp_runtime.registers.esp,
            TEST_ENTRY_ESP + 8u);
        passed &= expect_u32(
            "stencil enable dirty unchanged",
            *recomp_memory_u32(TEST_DIRTY_MASK),
            0x00004100u | TEST_DIRTY_BIT);
    }

    adapter = recomp_lookup_manual(0x001e6190u);
    if (adapter != recomp_d3d_set_z_enable_adapter ||
        recomp_lookup_manual(0x001e618fu) != NULL ||
        recomp_lookup_manual(0x001e6191u) != NULL) {
        fprintf(stderr, "D3D Z enable: lookup was not exact\n");
        passed = 0;
    } else {
        prepare_call(call_memory, 1u, 0x0010abcdu);
        adapter();
        passed &= expect_u32("Z enable adapter value", model->z_enable, 1u);
        passed &= expect_u32(
            "Z enable adapter count", model->z_enable_update_count, 1u);
        passed &= expect_u32(
            "Z enable shadow",
            *recomp_memory_u32(TEST_Z_ENABLE_SHADOW),
            1u);
        passed &= expect_u32(
            "Z enable ESP",
            recomp_runtime.registers.esp,
            TEST_ENTRY_ESP + 8u);
    }

    adapter = recomp_lookup_manual(0x001e5470u);
    if (adapter != recomp_d3d_set_fill_mode_adapter ||
        recomp_lookup_manual(0x001e546fu) != NULL ||
        recomp_lookup_manual(0x001e5471u) != NULL) {
        fprintf(stderr, "D3D fill mode: lookup was not exact\n");
        passed = 0;
    } else {
        prepare_call(call_memory, 0x1b02u, 0x0010abcdu);
        adapter();
        passed &= expect_u32(
            "fill mode adapter value", model->fill_mode, 0x1b02u);
        passed &= expect_u32(
            "fill mode adapter count", model->fill_mode_update_count, 1u);
        passed &= expect_u32(
            "fill mode shadow",
            *recomp_memory_u32(TEST_FILL_MODE_SHADOW),
            0x1b02u);
        passed &= expect_u32(
            "fill mode ESP",
            recomp_runtime.registers.esp,
            TEST_ENTRY_ESP + 8u);
    }

    adapter = recomp_lookup_manual(0x001e5080u);
    if (adapter != recomp_d3d_set_edge_antialias_adapter ||
        recomp_lookup_manual(0x001e507fu) != NULL ||
        recomp_lookup_manual(0x001e5081u) != NULL) {
        fprintf(stderr, "D3D edge AA: lookup was not exact\n");
        passed = 0;
    } else {
        prepare_call(call_memory, 0u, 0x0010abcdu);
        adapter();
        passed &= expect_u32(
            "edge AA adapter value", model->edge_antialias, 0u);
        passed &= expect_u32(
            "edge AA adapter count", model->edge_antialias_update_count, 1u);
        passed &= expect_u32(
            "edge AA shadow",
            *recomp_memory_u32(TEST_EDGE_ANTIALIAS_SHADOW),
            0u);
        passed &= expect_u32(
            "edge AA ESP",
            recomp_runtime.registers.esp,
            TEST_ENTRY_ESP + 8u);
    }

    adapter = recomp_lookup_manual(0x001e62b0u);
    if (adapter != recomp_d3d_set_stencil_fail_adapter ||
        recomp_lookup_manual(0x001e62afu) != NULL ||
        recomp_lookup_manual(0x001e62b1u) != NULL) {
        fprintf(stderr, "D3D stencil fail: lookup was not exact\n");
        passed = 0;
    } else {
        uint32_t value = 0u;
        uint32_t updates = model->simple_update_count;
        uint32_t neighbor = *recomp_memory_u32(TEST_STENCIL_FAIL_SHADOW + 4u);
        RecompD3dDepthState depth;

        prepare_call(call_memory, 0x150au, 0x0010abcdu);
        recomp_runtime.registers.esi = 0x12345678u;
        adapter();
        passed &= expect_u32("stencil fail method present",
            recomp_d3d_get_simple_render_state(model, 0x40370u, &value), 1u);
        passed &= expect_u32("stencil fail method value", value, 0x150au);
        passed &= expect_u32("stencil fail update count",
            model->simple_update_count, updates + 1u);
        passed &= expect_u32("stencil fail shadow",
            *recomp_memory_u32(TEST_STENCIL_FAIL_SHADOW), 0x150au);
        passed &= expect_u32("stencil fail following word",
            *recomp_memory_u32(TEST_STENCIL_FAIL_SHADOW + 4u), neighbor);
        passed &= expect_u32("stencil fail ESP",
            recomp_runtime.registers.esp, TEST_ENTRY_ESP + 8u);
        passed &= expect_u32("stencil fail argument preserved",
            *recomp_memory_u32(TEST_ENTRY_ESP + 4u), 0x150au);
        passed &= expect_u32("stencil fail ESI preserved",
            recomp_runtime.registers.esi, 0x12345678u);
        recomp_d3d_depth_state(model, &depth);
        passed &= expect_u32("stencil fail decoded",
            depth.stencil_fail, RECOMP_D3D_STENCIL_INVERT);
    }

    recomp_d3d_render_state_adapter_reset();
    passed &= expect_u32("adapter reset value", model->normalize_normals, 0u);
    passed &= expect_u32(
        "adapter reset count", model->normalize_normals_update_count, 0u);

    {
        /* Deferred parameters are float bits, including signed start values. */
        const uint32_t words[] = {1, 3, 0xc1200000u, 0x42c80000u, 0x3f000000u, 1};
        RecompD3dFogState fog;
        recomp_d3d_fog_state(words, 0x7f123456u, &fog);
        passed &= expect_u32("fog enable", fog.enabled, 1);
        passed &= expect_u32("fog range", fog.range, 1);
        passed &= expect_u32("fog mode", fog.mode, 3u);
        passed &= expect_u32("fog color", fog.color, 0x7f123456u);
        passed &= expect_u32("fog start", fog.start == -10.0f, 1);
        passed &= expect_u32("fog end", fog.end == 100.0f, 1);
        passed &= expect_u32("fog density", fog.density == 0.5f, 1);
        recomp_d3d_fog_state(words, 0, NULL);
        recomp_d3d_fog_state(NULL, 0, &fog);
        passed &= expect_u32("null fog input leaves output unchanged", fog.color, 0x7f123456u);
        uint32_t invalid[6];
        memcpy(invalid, words, sizeof invalid);
        invalid[1] = 4;
        recomp_d3d_fog_state(invalid, 0, &fog);
        passed &= expect_u32("unknown fog mode is disabled", fog.enabled, 0);
        invalid[1] = UINT32_MAX;
        recomp_d3d_fog_state(invalid, 0, &fog);
        passed &= expect_u32("invalid fog mode is disabled", fog.enabled, 0);
    }
    passed &= depth_state_test();
    passed &= stencil_state_test();
    passed &= texture_factor_selector_test();
    passed &= texture_factor_modulate_selector_test();
    passed &= texture_material_alpha_mode_test();
    {
        uint32_t stages[4][32] = {0};
        const uint32_t args[3][6] = {{2,2,1,4,2,0}, {14,2,1,14,2,1}, {4,0,1,4,0,1}};
        const uint32_t slots[] = {12,14,15,16,18,19};
        for (uint32_t s = 0; s < 3; ++s) {
            for (uint32_t i = 0; i < 6; ++i) stages[s][slots[i]] = args[s][i];
        }
        for (uint32_t s = 0; s < 2; ++s) {
            stages[s][0] = stages[s][1] = 1;
            stages[s][3] = stages[s][4] = 2;
        }
        stages[1][21] = 2; stages[1][28] = 0x30000; stages[3][12] = 1;
        passed &= expect_u32("reflection material", recomp_d3d_reflection_material(stages), 1);
        stages[1][21] = stages[1][28] = 0;
        passed &= expect_u32("environment material with mesh UV0", recomp_d3d_reflection_material(stages), 1);
        stages[1][28] = 1;
        passed &= expect_u32("unavailable mesh UV1 remains unsupported", recomp_d3d_reflection_material(stages), 0);
        stages[1][21] = 2;
        stages[1][28] = 2;
        passed &= expect_u32("stream UV2 is not generated reflection", recomp_d3d_reflection_material(stages), 0);
        stages[1][28] = 0x30000; stages[2][14] = 2;
        passed &= expect_u32("third sampled texture is not diffuse", recomp_d3d_reflection_material(stages), 0);
        stages[2][14] = 0; stages[1][12] = 15;
        passed &= expect_u32("factor alpha differs from texture alpha", recomp_d3d_reflection_material(stages), 0);
    }
    passed &= zero_diffuse_rgb_test();
    passed &= blend_state_test();

    return passed;
}
