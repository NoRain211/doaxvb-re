#include "d3d_frame_adapter.h"
#include "d3d_draw_adapter.h"
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

    recomp_d3d_frame_adapter_reset();
    if (recomp_d3d_presenter_memory_snapshot(&snapshot)) {
        fprintf(stderr, "D3D frame adapter: reset left a presenter\n");
        passed = 0;
    }
    return passed;
}

/* Each close case runs in its own process and must exit inside the draw seam. */
int recomp_d3d_draw_presenter_close_test(int indexed)
{
    static uint8_t device_memory[TEST_DEVICE_SIZE];
    static uint8_t call_memory[TEST_CALL_SIZE];
    const RecompMemoryRegion regions[] = {
        {TEST_DEVICE_BASE, sizeof device_memory, device_memory},
        {TEST_CALL_BASE, sizeof call_memory, call_memory},
    };
    const RecompD3dPresenterConfig config = {
        4u, 4u, RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
        RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8,
    };
    const uint32_t vertices = TEST_CALL_BASE + 0x800u;
    const uint32_t indices = TEST_CALL_BASE + 0x900u;
    const uint32_t buffer = TEST_DEVICE_BASE + 0x6100u;
    const float quad[4][6] = {{0,0,0,1,0,0}, {4,0,0,1,1,0},
        {0,4,0,1,0,1}, {4,4,0,1,1,1}};
    const uint16_t order[] = {0,1,2,3};
    const uint32_t args[] = {RECOMP_D3D_PT_TRIANGLESTRIP, 4u,
        indexed ? indices : vertices, sizeof quad[0]};
    recomp_runtime_init(regions, 2u, NULL, 0u, NULL, 0u);
    recomp_d3d_frame_adapter_initialize(&config, TEST_DEVICE);
    *recomp_memory_u32(0x001f2978u) = TEST_DEVICE;
    *recomp_memory_u32(TEST_DEVICE + 0x21b4u) = buffer;
    *recomp_memory_u32(TEST_DEVICE + 0x21c0u) = buffer;
    *recomp_memory_u32(TEST_DEVICE + 0x384u) = indexed ? 0x002u : 0x104u;
    *recomp_memory_u32(0x001f2e20u) = sizeof quad[0];
    *recomp_memory_u32(0x001f2e28u) = buffer;
    *recomp_memory_u32(buffer + 4u) = vertices;
    for (unsigned transform = 0u; transform <= 6u; ++transform) {
        float identity[16] = {0};
        identity[0] = identity[5] = identity[10] = identity[15] = 1;
        memcpy(recomp_memory_u32(TEST_DEVICE + 0x810u + transform * 0x40u),
            identity, sizeof identity);
    }
    prepare_stack(call_memory, args, indexed ? 3u : 4u);
    memcpy(recomp_memory_u32(vertices), quad, sizeof quad);
    memcpy(recomp_memory_u32(indices), order, sizeof order);
    recomp_d3d_presenter_memory_set_error(RECOMP_D3D_PRESENTER_CLOSED);
    RecompFunction draw = recomp_d3d_draw_lookup_manual(indexed ? 0x001e78b0u : 0x001e7750u);
    if (draw == NULL) {
        fprintf(stderr, "FAIL draw lookup failed for %s\n", indexed ? "indexed" : "UP");
        return 1;
    }
    draw();
    fprintf(stderr, "FAIL closed presenter returned from %s draw\n", indexed ? "indexed" : "UP");
    return 1;
}
