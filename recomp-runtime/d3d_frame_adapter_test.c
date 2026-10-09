#include "d3d_frame_adapter.h"
#include "d3d_draw_model.h"
#include "d3d_presenter_memory_test.h"
#include "d3d_vblank.h"
#include "xapi_time_adapter.h"
#include "program_manual.h"
#include "runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    TEST_DEVICE_BASE = 0x001f0000u,
    TEST_DEVICE_SIZE = 0x00007000u,
    TEST_CALL_BASE = 0x28000000u,
    TEST_CALL_SIZE = 0x00001000u,
    TEST_ENTRY_ESP = TEST_CALL_BASE + 0x100u,
    TEST_DEVICE = 0x001f3120u,
    TEST_KERNEL_DATA_BASE = 0x00740000u,
    TEST_KERNEL_DATA_SIZE = 0x00001000u,
    TEST_KE_TICK_COUNT = TEST_KERNEL_DATA_BASE + 0x40u,
};

static int expect_u32(
    const char *field,
    uint32_t actual,
    uint32_t expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(
        stderr,
        "D3D frame adapter: %s was 0x%08x, expected 0x%08x\n",
        field,
        actual,
        expected);
    return 0;
}

static void prepare_stack(uint8_t *call_memory, const uint32_t *args, size_t count)
{
    uint32_t *stack = (uint32_t *)(void *)(
        call_memory + TEST_ENTRY_ESP - TEST_CALL_BASE);

    memset(call_memory, 0, TEST_CALL_SIZE);
    stack[0] = 0xdeadbeefu;
    for (size_t i = 0u; i < count; ++i) {
        stack[i + 1u] = args[i];
    }
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
}

int recomp_d3d_frame_adapter_test(void)
{
    static uint8_t device_memory[TEST_DEVICE_SIZE];
    static uint8_t call_memory[TEST_CALL_SIZE];
    static uint8_t kernel_data_memory[TEST_KERNEL_DATA_SIZE];
    const RecompMemoryRegion regions[] = {
        {
            .address = TEST_DEVICE_BASE,
            .size = sizeof device_memory,
            .data = device_memory,
        },
        {
            .address = TEST_CALL_BASE,
            .size = sizeof call_memory,
            .data = call_memory,
        },
        {
            .address = TEST_KERNEL_DATA_BASE,
            .size = sizeof kernel_data_memory,
            .data = kernel_data_memory,
        },
    };
    const uint32_t clear_args[] = {
        0u,
        0u,
        0xf3u,
        0x10203040u,
        0x3f800000u,
        0x2au,
    };
    const uint32_t swap_args[] = {0u};
    const RecompD3dPresenterConfig config = {
        .width = 720u,
        .height = 480u,
        .color_format = RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
        .depth_format = RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8,
    };
    RecompD3dPresenterMemorySnapshot snapshot;
    RecompFunction clear;
    RecompFunction swap;
    uint32_t clear_z_bits;
    int passed = 1;

    memset(device_memory, 0, sizeof device_memory);
    memset(kernel_data_memory, 0, sizeof kernel_data_memory);
    recomp_runtime_init(regions, 3u, NULL, 0u, NULL, 0u);
    recomp_d3d_frame_adapter_reset();
    recomp_d3d_frame_adapter_initialize(&config, TEST_DEVICE);
    *recomp_memory_u32(TEST_DEVICE + 0x21b4u) = TEST_DEVICE_BASE + 0x6000u;
    *recomp_memory_u32(TEST_DEVICE + 0x21c0u) = TEST_DEVICE_BASE + 0x6000u;
    *recomp_memory_u32(TEST_DEVICE + 0x21b8u) = TEST_DEVICE_BASE + 0x6020u;
    *recomp_memory_u32(TEST_DEVICE + 0x21ccu) = TEST_DEVICE_BASE + 0x6020u;

    clear = recomp_d3d_frame_lookup_manual(0x001e72d0u);
    swap = recomp_d3d_frame_lookup_manual(0x001e8f30u);
    if (clear == NULL || swap == NULL) {
        fprintf(stderr, "D3D frame adapter: exact lookup failed\n");
        return 0;
    }
    if (recomp_d3d_frame_lookup_manual(0x001e72cfu) != NULL ||
        recomp_d3d_frame_lookup_manual(0x001e72d1u) != NULL ||
        recomp_d3d_frame_lookup_manual(0x001e8f2fu) != NULL ||
        recomp_d3d_frame_lookup_manual(0x001e8f31u) != NULL) {
        fprintf(stderr, "D3D frame adapter: adjacent lookup resolved\n");
        passed = 0;
    }
    if (recomp_lookup_manual(0x001e72d0u) != clear ||
        recomp_lookup_manual(0x001e8f30u) != swap) {
        fprintf(stderr, "D3D frame adapter: manual lookup chain failed\n");
        passed = 0;
    }


    prepare_stack(call_memory, clear_args, 6u);
    recomp_runtime.registers.eax = 0xa5a5a5a5u;
    clear();
    passed &= expect_u32(
        "Clear ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 28u);
    passed &= expect_u32(
        "Clear EAX", recomp_runtime.registers.eax, 0xa5a5a5a5u);
    if (!recomp_d3d_presenter_memory_snapshot(&snapshot)) {
        fprintf(stderr, "D3D frame adapter: Clear snapshot unavailable\n");
        passed = 0;
    } else {
        passed &= expect_u32("Clear command count", snapshot.command_count, 1u);
        passed &= expect_u32(
            "Clear command type",
            snapshot.commands[0].type,
            RECOMP_D3D_PRESENTER_COMMAND_CLEAR);
        passed &= expect_u32(
            "Clear color",
            snapshot.commands[0].data.clear.color,
            0x10203040u);
        memcpy(
            &clear_z_bits,
            &snapshot.commands[0].data.clear.z,
            sizeof clear_z_bits);
        passed &= expect_u32("Clear z bits", clear_z_bits, 0x3f800000u);
        passed &= expect_u32(
            "Clear stencil", snapshot.commands[0].data.clear.stencil, 0x2au);
        passed &= expect_u32(
            "Clear back buffer", snapshot.commands[0].data.clear.target.offscreen, 0u);
        passed &= expect_u32(
            "Clear depth attached", snapshot.commands[0].data.clear.target.no_depth, 0u);
        passed &= expect_u32(
            "Clear default depth", snapshot.commands[0].data.clear.target.custom_depth, 0u);
    }

    prepare_stack(call_memory, swap_args, 1u);
    recomp_runtime.registers.eax = 0xccccccccu;
    swap();
    passed &= expect_u32(
        "Swap ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 8u);
    passed &= expect_u32("Swap EAX", recomp_runtime.registers.eax, 1u);
    passed &= expect_u32(
        "guest swap counter",
        *recomp_memory_u32(TEST_DEVICE + 0x2c10u),
        1u);
    passed &= expect_u32(
        "KeTickCount",
        *recomp_memory_u32(TEST_KE_TICK_COUNT),
        16u);
    if (!recomp_d3d_presenter_memory_snapshot(&snapshot)) {
        fprintf(stderr, "D3D frame adapter: Swap snapshot unavailable\n");
        passed = 0;
    } else {
        passed &= expect_u32("Swap command count", snapshot.command_count, 2u);
        passed &= expect_u32(
            "Swap effective flags",
            snapshot.commands[1].data.present.effective_flags,
            5u);
        passed &= expect_u32(
            "Swap command counter",
            snapshot.commands[1].data.present.swap_counter,
            1u);
    }

    {
        const uint32_t surface = TEST_DEVICE_BASE + 0x6200u;
        const uint32_t depth_surface = TEST_DEVICE_BASE + 0x6240u;
        const uint32_t default_depth = TEST_DEVICE_BASE + 0x6020u;
        RecompD3dPresenterTarget target;

        *recomp_memory_u32(TEST_DEVICE + 0x21b4u) = surface;
        *recomp_memory_u32(TEST_DEVICE + 0x21b8u) = 0u;
        *recomp_memory_u32(surface) = 0x01050001u;
        *recomp_memory_u32(surface + 4u) = 0x80010000u;
        *recomp_memory_u32(surface + 0xcu) = 0x07800600u;
        *recomp_memory_u32(surface + 0x10u) = 0u;
        device_memory[0x16b8u + 6u] = 0xa1u;
        prepare_stack(call_memory, clear_args, 6u);
        clear();
        if (!recomp_d3d_presenter_memory_snapshot(&snapshot)) {
            passed = 0;
        } else {
            target = snapshot.commands[2].data.clear.target;
            passed &= expect_u32("offscreen clear", target.offscreen, 1u);
            passed &= expect_u32("offscreen no depth", target.no_depth, 1u);
            passed &= expect_u32("offscreen null depth is not custom", target.custom_depth, 0u);
            passed &= expect_u32("offscreen data", target.color.data, 0x10000u);
            passed &= expect_u32("offscreen width", target.color.width, 256u);
            passed &= expect_u32("offscreen height", target.color.height, 128u);
        }
        *recomp_memory_u32(depth_surface + 4u) = 0x80020000u;
        *recomp_memory_u32(depth_surface + 0xcu) = 0x07802a00u;
        *recomp_memory_u32(depth_surface + 0x10u) = 0u;
        device_memory[0x16b8u + 0x2au] = 0xe1u;
        *recomp_memory_u32(TEST_DEVICE + 0x21b8u) = depth_surface;
        passed &= expect_u32(
            "custom depth accepted", recomp_d3d_frame_adapter_target(&target), 1u);
        passed &= expect_u32("custom depth attached", target.no_depth, 0u);
        passed &= expect_u32("custom depth selected", target.custom_depth, 1u);
        passed &= expect_u32("custom depth data", target.depth.data, 0x20000u);
        passed &= expect_u32("custom depth format", target.depth.format_byte, 0x2au);
        passed &= expect_u32("custom depth width", target.depth.width, 256u);
        passed &= expect_u32("custom depth height", target.depth.height, 128u);
        memcpy(recomp_memory_u32(default_depth), recomp_memory_u32(depth_surface), 0x14u);
        passed &= expect_u32(
            "aliased default depth accepted", recomp_d3d_frame_adapter_target(&target), 1u);
        passed &= expect_u32("aliased default depth reused", target.custom_depth, 0u);
        *recomp_memory_u32(depth_surface + 0xcu) = 0x08802a00u;
        passed &= expect_u32(
            "different depth shape accepted", recomp_d3d_frame_adapter_target(&target), 1u);
        passed &= expect_u32("different depth shape stays custom", target.custom_depth, 1u);
        *recomp_memory_u32(depth_surface + 0xcu) = 0x07800600u;
        passed &= expect_u32(
            "color as depth rejected", recomp_d3d_frame_adapter_target(&target), 0u);
        *recomp_memory_u32(depth_surface + 0xcu) = 0x07802a00u;
        *recomp_memory_u32(depth_surface + 4u) = 0u;
        passed &= expect_u32(
            "missing depth data rejected", recomp_d3d_frame_adapter_target(&target), 0u);
        *recomp_memory_u32(TEST_DEVICE + 0x21b4u) = 0u;
        passed &= expect_u32(
            "missing target rejected", recomp_d3d_frame_adapter_target(&target), 0u);
    }

    {
        const uint64_t before = recomp_xapi_performance_counter();
        recomp_d3d_vblank_reset();
        for (unsigned i = 0; i < 3; ++i) {
            prepare_stack(call_memory, swap_args, 1u);
            swap();
        }
        const uint64_t elapsed = recomp_xapi_performance_counter() - before;
        passed &= expect_u32("Swap paces without host presentation blocking",
            elapsed >= recomp_xapi_performance_frequency() / 20u - 3u, 1u);
    }

    {
        const uint32_t args[] = {1u, TEST_CALL_BASE + 0x800u};
        RecompFunction gamma = recomp_d3d_frame_lookup_manual(0x001e3640u);
        uint8_t expected[768];
        for (size_t i = 0u; i < sizeof expected; ++i) expected[i] = (uint8_t)(i * 7u);
        prepare_stack(call_memory, args, 2u);
        memcpy(call_memory + 0x800u, expected, sizeof expected);
        if (gamma == NULL || recomp_lookup_manual(0x001e3640u) != gamma) return 0;
        gamma();
        memset(call_memory + 0x800u, 0, sizeof expected);
        passed &= expect_u32("Gamma ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 12u);
        if (!recomp_d3d_presenter_memory_snapshot(&snapshot) || snapshot.command_count == 0u) return 0;
        const RecompD3dPresenterCommand *command = &snapshot.commands[snapshot.command_count-1u];
        passed &= expect_u32("Gamma command", command->type, RECOMP_D3D_PRESENTER_COMMAND_GAMMA);
        passed &= memcmp(command->data.gamma, expected, sizeof expected) == 0;
    }

    /* A failed transform/state read must not submit partially enabled fog. */
    recomp_d3d_frame_adapter_reset();
    recomp_d3d_frame_adapter_initialize(&config, TEST_DEVICE);
    *recomp_memory_u32(0x001f2978u) = TEST_DEVICE;
    *recomp_memory_u32(TEST_DEVICE + 0x384u) = 0x104u;
    *recomp_memory_u32(TEST_DEVICE + 0x21b4u) = TEST_DEVICE_BASE + 0x6000u;
    *recomp_memory_u32(TEST_DEVICE + 0x21c0u) = TEST_DEVICE_BASE + 0x6000u;
    *recomp_memory_u32(TEST_DEVICE + 0x21b8u) = 0;
    const uint32_t fog_words[] = {1,3,0,0x40800000u,0,1};
    memcpy(recomp_memory_u32(0x001f2b88u + 92u*4u), fog_words, sizeof fog_words);
    *recomp_memory_u32(0x001f2b88u + 138u*4u) = 0x000000ffu;
    float identity[16] = {0};
    for (unsigned i = 0; i < 4; ++i) identity[i*5] = 1;
    const uint32_t slots[] = {0,1,6};
    for (unsigned i = 0; i < 3; ++i)
        memcpy(recomp_memory_u32(TEST_DEVICE + 0x810u + slots[i]*0x40u), identity, sizeof identity);
    const float vertices[][6] = {{0,0,0.25f,1,0,0},{4,0,0.25f,1,0,0},
        {0,4,0.25f,1,0,0},{4,4,0.25f,1,0,0}};
    const uint32_t draw_args[] = {RECOMP_D3D_PT_TRIANGLESTRIP,4,TEST_CALL_BASE + 0x400u,sizeof vertices[0]};
    RecompFunction draw = recomp_lookup_manual(0x001e7750u);
    if (draw == NULL) return 0;
    const uint32_t holes[] = {0, TEST_DEVICE + 0x850u, TEST_DEVICE + 0x810u,
        TEST_DEVICE + 0x990u, 0x001f2b88u + 92u*4u, 0x001f2b88u + 138u*4u};
    for (unsigned i = 0; i < sizeof holes / sizeof holes[0]; ++i) {
        RecompMemoryRegion split[4] = {regions[0],regions[0],regions[1],regions[2]};
        if (holes[i]) {
            split[0].size = holes[i] - TEST_DEVICE_BASE;
            split[1].address = holes[i] + 4u;
            split[1].size = TEST_DEVICE_BASE + TEST_DEVICE_SIZE - split[1].address;
            split[1].data = device_memory + split[1].address - TEST_DEVICE_BASE;
            recomp_runtime.memory_regions = split;
            recomp_runtime.memory_region_count = 4;
        }
        prepare_stack(call_memory, draw_args, 4);
        memcpy(call_memory + 0x400u, vertices, sizeof vertices);
        draw();
        recomp_runtime.memory_regions = regions;
        recomp_runtime.memory_region_count = 3;
        if (!recomp_d3d_presenter_memory_snapshot(&snapshot)) return 0;
        passed &= expect_u32("fog draw submitted", snapshot.draw_count, i + 1u);
        const RecompD3dPresenterDrawCommand *captured = &snapshot.commands[i].data.draw;
        passed &= expect_u32("fog requires all retained state and transforms", captured->fog.enabled, i == 0);
        passed &= expect_u32("failed fog read clears Z mode", captured->fog_z, i == 0);
        passed &= expect_u32("failed fog read clears range matrices", captured->fog_world_view[0][0] == 1, i == 0);
    }
    recomp_d3d_frame_adapter_reset();
    if (recomp_d3d_presenter_memory_snapshot(&snapshot)) {
        fprintf(stderr, "D3D frame adapter: reset left a presenter\n");
        passed = 0;
    }
    return passed;
}
