#include "runtime.h"
#include "program_manual.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

void sub_00189800(void);
int recomp_xapi_time_adapter_test(void);
int recomp_device_model_test(void);
int recomp_d3d_creation_model_test(void);
int recomp_d3d_draw_model_test(void);
int recomp_d3d_draw_adapter_test(void);
int recomp_d3d_frame_model_test(void);
int recomp_d3d_frame_adapter_test(void);
int recomp_d3d_presenter_memory_test(void);
int recomp_d3d_render_state_model_test(void);
int recomp_d3d_texture_model_test(void);
int recomp_d3d_tile_model_test(void);
int recomp_d3d_vertex_shader_model_test(void);
int recomp_dsound_service_adapter_test(void);
int recomp_custom_music_test(void); /* process exit code; 77 = skipped */
int recomp_cri_service_model_test(void);
int recomp_cri_service_adapter_test(void);
int recomp_crt_format_adapter_test(void);
int recomp_crt_string_adapter_test(void);
int recomp_animation_track_adapter_test(void);
int recomp_fiber_adapter_test(void);
int recomp_fiber_model_test(void);
int recomp_flag_macro_test(void);
int recomp_sse_semantics_test(void);
int recomp_directory_model_test(void);
int recomp_input_model_test(void);
int recomp_input_pulse_source_test(void);
int recomp_symbolic_link_model_test(void);
int recomp_ohci_model_test(void);
int recomp_apu_model_test(void);
int recomp_kernel_memory_test(void);
int recomp_kernel_allocation_test(void);
int recomp_kernel_presenter_close_test(void);
int recomp_d3d_draw_presenter_close_test(int indexed);
int recomp_kernel_file_save_test(void);
int recomp_kernel_thread_test(void);
int recomp_kernel_video_test(void);
int recomp_kernel_rtl_test(void);
int recomp_kernel_crypto_test(void);

typedef struct LeafFixture {
    const char *name;
    RecompRegisters initial;
    uint32_t stack_address;
    uint32_t object_address;
    uint32_t object_value;
} LeafFixture;

static const LeafFixture fixtures[] = {
    {
        .name = "case01",
        .initial = {
            0xa5a5a5a5u, 0x11111111u, 0x22222222u, 0x33333333u,
            0x44444444u, 0x55555555u, 0x66666666u, 0x25001000u,
        },
        .stack_address = 0x25001000u,
        .object_address = 0x26001000u,
        .object_value = 0x00000000u,
    },
    {
        .name = "case02",
        .initial = {
            0x00000000u, 0x12121212u, 0x23232323u, 0x34343434u,
            0x45454545u, 0x56565656u, 0x67676767u, 0x25002000u,
        },
        .stack_address = 0x25002000u,
        .object_address = 0x26002000u,
        .object_value = 0xffffffffu,
    },
    {
        .name = "case03",
        .initial = {
            0x12345678u, 0x13131313u, 0x24242424u, 0x35353535u,
            0x46464646u, 0x57575757u, 0x68686868u, 0x25003000u,
        },
        .stack_address = 0x25003000u,
        .object_address = 0x26003000u,
        .object_value = 0x80000000u,
    },
    {
        .name = "case04",
        .initial = {
            0xffffffffu, 0x14141414u, 0x25252525u, 0x36363636u,
            0x47474747u, 0x58585858u, 0x69696969u, 0x25004000u,
        },
        .stack_address = 0x25004000u,
        .object_address = 0x26000fe0u,
        .object_value = 0x7fffffffu,
    },
};

static const RecompFunctionEntry functions[] = {
    {0x00189800u, sub_00189800},
};

static void fill_object_window(
    uint8_t *bytes,
    uint32_t address,
    size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = (uint8_t)(((address + (uint32_t)i) * 33u + 17u) &
                             0xffu);
    }
}

static int expect_u32(
    const char *case_name,
    const char *field,
    uint32_t actual,
    uint32_t expected)
{
    if (actual == expected) {
        return 1;
    }

    fprintf(
        stderr,
        "%s: %s was 0x%08x, expected 0x%08x\n",
        case_name,
        field,
        actual,
        expected);
    return 0;
}

static int expect_size(
    const char *case_name,
    const char *field,
    size_t actual,
    size_t expected)
{
    if (actual == expected) {
        return 1;
    }

    fprintf(
        stderr,
        "%s: %s was %zu, expected %zu\n",
        case_name,
        field,
        actual,
        expected);
    return 0;
}

static int run_fixture(const LeafFixture *fixture)
{
    uint32_t stack_value = fixture->object_address;
    uint32_t stack_before = stack_value;
    uint32_t object_words[8];
    uint32_t object_before[ARRAY_SIZE(object_words)];
    RecompMemoryAccess accesses[2];
    RecompMemoryRegion regions[] = {
        {
            .address = fixture->stack_address + 4u,
            .size = sizeof(stack_value),
            .data = (uint8_t *)&stack_value,
        },
        {
            .address = fixture->object_address,
            .size = sizeof(object_words),
            .data = (uint8_t *)object_words,
        },
    };
    RecompRegisters expected = fixture->initial;
    int passed = 1;

    fill_object_window(
        (uint8_t *)object_words,
        fixture->object_address,
        sizeof(object_words));
    memcpy(
        (uint8_t *)object_words + 0x1cu,
        &fixture->object_value,
        sizeof(fixture->object_value));
    memcpy(object_before, object_words, sizeof(object_words));

    recomp_runtime_init(
        regions,
        ARRAY_SIZE(regions),
        accesses,
        ARRAY_SIZE(accesses),
        functions,
        ARRAY_SIZE(functions));
    recomp_runtime.registers = fixture->initial;

    if (!recomp_dispatch(0x00189800u)) {
        fprintf(stderr, "%s: dispatch failed\n", fixture->name);
        return 0;
    }

    expected.eax = fixture->object_value;
    expected.esp = fixture->stack_address + 4u;
    passed &= expect_u32(
        fixture->name, "eax", recomp_runtime.registers.eax, expected.eax);
    passed &= expect_u32(
        fixture->name, "ecx", recomp_runtime.registers.ecx, expected.ecx);
    passed &= expect_u32(
        fixture->name, "edx", recomp_runtime.registers.edx, expected.edx);
    passed &= expect_u32(
        fixture->name, "ebx", recomp_runtime.registers.ebx, expected.ebx);
    passed &= expect_u32(
        fixture->name, "esi", recomp_runtime.registers.esi, expected.esi);
    passed &= expect_u32(
        fixture->name, "edi", recomp_runtime.registers.edi, expected.edi);
    passed &= expect_u32(
        fixture->name, "ebp", recomp_runtime.registers.ebp, expected.ebp);
    passed &= expect_u32(
        fixture->name, "esp", recomp_runtime.registers.esp, expected.esp);

    passed &= expect_size(
        fixture->name, "access_count", recomp_runtime.access_count, 2u);
    passed &= expect_size(
        fixture->name,
        "undeclared_access_count",
        recomp_runtime.undeclared_access_count,
        0u);
    if (recomp_runtime.access_count == 2u) {
        passed &= expect_u32(
            fixture->name,
            "read[0].address",
            accesses[0].address,
            fixture->stack_address + 4u);
        passed &= expect_u32(
            fixture->name, "read[0].width", accesses[0].width, 4u);
        passed &= expect_u32(
            fixture->name,
            "read[1].address",
            accesses[1].address,
            fixture->object_address + 0x1cu);
        passed &= expect_u32(
            fixture->name, "read[1].width", accesses[1].width, 4u);
    }

    if (stack_value != stack_before ||
        memcmp(object_words, object_before, sizeof(object_words)) != 0) {
        fprintf(stderr, "%s: readable guest memory changed\n", fixture->name);
        passed = 0;
    }

    return passed;
}

static int run_invalid_access(void)
{
    uint32_t word = 0;
    RecompMemoryAccess access;
    const RecompMemoryRegion region = {
        .address = 0x00001000u,
        .size = sizeof(word),
        .data = (uint8_t *)&word,
    };

    recomp_runtime_init(&region, 1u, &access, 1u, NULL, 0u);
    (void)*recomp_memory_u32(0xffffffffu);
    return EXIT_SUCCESS;
}

static int run_cached_ram_alias(void)
{
    uint32_t words[] = {0x11223344u, 0x55667788u};
    RecompMemoryAccess accesses[2];
    const RecompMemoryRegion region = {
        .address = 0x01c7f7e0u,
        .size = sizeof(words),
        .data = (uint8_t *)words,
    };
    int passed = 1;

    recomp_runtime_init(
        &region, 1u, accesses, ARRAY_SIZE(accesses), NULL, 0u);
    passed &= expect_u32(
        "cached RAM alias",
        "direct read",
        *recomp_memory_u32(0x01c7f7e4u),
        0x55667788u);
    *recomp_memory_u32(0x81c7f7e4u) = 0xaabbccddu;
    passed &= expect_u32(
        "cached RAM alias", "aliased write", words[1], 0xaabbccddu);
    passed &= expect_size(
        "cached RAM alias", "access_count", recomp_runtime.access_count, 2u);
    passed &= expect_size(
        "cached RAM alias",
        "undeclared_access_count",
        recomp_runtime.undeclared_access_count,
        0u);
    if (recomp_runtime.access_count == 2u) {
        passed &= expect_u32(
            "cached RAM alias",
            "direct access address",
            accesses[0].address,
            0x01c7f7e4u);
        passed &= expect_u32(
            "cached RAM alias",
            "alias access address",
            accesses[1].address,
            0x81c7f7e4u);
    }
    return passed;
}

static int run_physical_ram_alias(void)
{
    uint32_t words[] = {0x11223344u, 0x55667788u};
    RecompMemoryAccess accesses[2];
    const RecompMemoryRegion region = {
        .address = 0x0368bffcu,
        .size = sizeof(words),
        .data = (uint8_t *)words,
    };
    int passed = 1;

    recomp_runtime_init(
        &region, 1u, accesses, ARRAY_SIZE(accesses), NULL, 0u);
    passed &= expect_u32(
        "physical RAM alias",
        "direct read",
        *recomp_memory_u32(0x0368c000u),
        0x55667788u);
    *recomp_memory_u32(0xf368c000u) = 0xaabbccddu;
    passed &= expect_u32(
        "physical RAM alias", "aliased write", words[1], 0xaabbccddu);
    passed &= expect_size(
        "physical RAM alias", "access_count", recomp_runtime.access_count, 2u);
    passed &= expect_size(
        "physical RAM alias",
        "undeclared_access_count",
        recomp_runtime.undeclared_access_count,
        0u);
    if (recomp_runtime.access_count == 2u) {
        passed &= expect_u32(
            "physical RAM alias",
            "direct access address",
            accesses[0].address,
            0x0368c000u);
        passed &= expect_u32(
            "physical RAM alias",
            "alias access address",
            accesses[1].address,
            0xf368c000u);
    }
    return passed;
}

static int run_usb0_ohci_initialization(void)
{
    static const uint32_t expected_addresses[] = {
        0xfed00000u,
        0xfed00004u,
        0xfed00048u,
        0xfed0004cu,
        0xfed00050u,
        0xfed00048u,
        0xfed00050u,
        0xfed00008u,
        0xfed00008u,
        0xfed00008u,
        0xfed00008u,
        0xfed00018u,
        0xfed00018u,
        0xfed0001cu,
        0xfed0001cu,
        0xfed00020u,
        0xfed00020u,
        0xfed00024u,
        0xfed00024u,
        0xfed00028u,
        0xfed00028u,
        0xfed0002cu,
        0xfed0002cu,
        0xfed00030u,
        0xfed00030u,
        0xfed00040u,
        0xfed00040u,
        0xfed00044u,
        0xfed00044u,
        0xfed00004u,
        0xfed00004u,
        0xfed00034u,
        0xfed00034u,
        0xfed00034u,
        0xfed00010u,
        0xfed00010u,
        0xfed00010u,
        0xfed00050u,
        0xfed00050u,
        0xfed00050u,
        0xfed00054u,
        0xfed00054u,
        0xfed00054u,
        0xfed00058u,
        0xfed00058u,
        0xfed00058u,
        0xfed0005cu,
        0xfed0005cu,
        0xfed0005cu,
        0xfed00060u,
        0xfed00060u,
        0xfed00060u,
        0xfed00010u,
        0xfed00010u,
    };
    RecompMemoryAccess accesses[ARRAY_SIZE(expected_addresses)];
    uint32_t command_after_reset;
    uint32_t command_initial;
    uint32_t command_repeat;
    uint32_t control;
    uint32_t control_current_ed;
    uint32_t control_head_ed;
    uint32_t control_operational;
    uint32_t descriptor_a;
    uint32_t done_head;
    uint32_t bulk_current_ed;
    uint32_t bulk_head_ed;
    uint32_t fm_interval;
    uint32_t fm_interval_initial;
    uint32_t hcca;
    uint32_t interrupt_enable_first;
    uint32_t interrupt_enable_initial;
    uint32_t interrupt_enable_second;
    uint32_t ls_threshold;
    uint32_t period_current_ed;
    uint32_t periodic_start;
    uint32_t revision;
    uint32_t root_port_status[4];
    uint32_t root_port_status_after[4];
    uint32_t rh_status_reset;
    uint32_t rh_status_reset_after;
    uint32_t status;
    int passed = 1;

    recomp_runtime_init(
        NULL, 0u, accesses, ARRAY_SIZE(accesses), NULL, 0u);
    revision = *recomp_memory_u32(0xfed00000u);
    control = *recomp_memory_u32(0xfed00004u);
    *recomp_memory_u32(0xfed00048u) = 0x00001200u;
    *recomp_memory_u32(0xfed0004cu) = 0u;
    *recomp_memory_u32(0xfed00050u) = 0x80000000u;
    descriptor_a = *recomp_memory_u32(0xfed00048u);
    status = *recomp_memory_u32(0xfed00050u);
    command_initial = *recomp_memory_u32(0xfed00008u);
    *recomp_memory_u32(0xfed00008u) = 0x00000001u;
    command_after_reset = *recomp_memory_u32(0xfed00008u);
    command_repeat = *recomp_memory_u32(0xfed00008u);
    *recomp_memory_u32(0xfed00018u) = 0x03fde000u;
    hcca = *recomp_memory_u32(0xfed00018u);
    *recomp_memory_u32(0xfed0001cu) = 0u;
    period_current_ed = *recomp_memory_u32(0xfed0001cu);
    *recomp_memory_u32(0xfed00020u) = 0u;
    control_head_ed = *recomp_memory_u32(0xfed00020u);
    *recomp_memory_u32(0xfed00024u) = 0u;
    control_current_ed = *recomp_memory_u32(0xfed00024u);
    *recomp_memory_u32(0xfed00028u) = 0u;
    bulk_head_ed = *recomp_memory_u32(0xfed00028u);
    *recomp_memory_u32(0xfed0002cu) = 0u;
    bulk_current_ed = *recomp_memory_u32(0xfed0002cu);
    *recomp_memory_u32(0xfed00030u) = 0u;
    done_head = *recomp_memory_u32(0xfed00030u);
    *recomp_memory_u32(0xfed00040u) = 0x00002a29u;
    periodic_start = *recomp_memory_u32(0xfed00040u);
    *recomp_memory_u32(0xfed00044u) = 0x00000620u;
    ls_threshold = *recomp_memory_u32(0xfed00044u);
    *recomp_memory_u32(0xfed00004u) = 0x000000beu;
    control_operational = *recomp_memory_u32(0xfed00004u);
    fm_interval_initial = *recomp_memory_u32(0xfed00034u);
    *recomp_memory_u32(0xfed00034u) = 0xa7722ed8u;
    fm_interval = *recomp_memory_u32(0xfed00034u);
    interrupt_enable_initial = *recomp_memory_u32(0xfed00010u);
    *recomp_memory_u32(0xfed00010u) = 0x80000033u;
    interrupt_enable_first = *recomp_memory_u32(0xfed00010u);
    rh_status_reset = *recomp_memory_u32(0xfed00050u);
    *recomp_memory_u32(0xfed00050u) = 0u;
    rh_status_reset_after = *recomp_memory_u32(0xfed00050u);
    for (size_t i = 0u; i < ARRAY_SIZE(root_port_status); ++i) {
        uint32_t address = 0xfed00054u + (uint32_t)(i * 4u);

        root_port_status[i] = *recomp_memory_u32(address);
        *recomp_memory_u32(address) = 0u;
        root_port_status_after[i] = *recomp_memory_u32(address);
    }
    *recomp_memory_u32(0xfed00010u) = 0x00000040u;
    interrupt_enable_second = *recomp_memory_u32(0xfed00010u);

    passed &= expect_u32(
        "USB0 OHCI", "HcRevision", revision, 0x00000010u);
    passed &= expect_u32(
        "USB0 OHCI", "HcControl", control, 0x00000000u);
    passed &= expect_u32(
        "USB0 OHCI", "HcRhDescriptorA", descriptor_a, 0x00000204u);
    passed &= expect_u32(
        "USB0 OHCI", "HcRhStatus", status, 0x00000000u);
    passed &= expect_u32(
        "USB0 OHCI", "initial HcCommandStatus", command_initial, 0u);
    passed &= expect_u32(
        "USB0 OHCI",
        "post-reset HcCommandStatus",
        command_after_reset,
        0u);
    passed &= expect_u32(
        "USB0 OHCI", "repeated HcCommandStatus", command_repeat, 0u);
    passed &= expect_u32(
        "USB0 OHCI", "HcHCCA readback", hcca, 0x03fde000u);
    passed &= expect_u32(
        "USB0 OHCI", "HcPeriodCurrentED", period_current_ed, 0u);
    passed &= expect_u32(
        "USB0 OHCI", "HcControlHeadED", control_head_ed, 0u);
    passed &= expect_u32(
        "USB0 OHCI", "HcControlCurrentED", control_current_ed, 0u);
    passed &= expect_u32(
        "USB0 OHCI", "HcBulkHeadED", bulk_head_ed, 0u);
    passed &= expect_u32(
        "USB0 OHCI", "HcBulkCurrentED", bulk_current_ed, 0u);
    passed &= expect_u32("USB0 OHCI", "HcDoneHead", done_head, 0u);
    passed &= expect_u32(
        "USB0 OHCI", "initial HcFmInterval", fm_interval_initial, 0x00002edfu);
    passed &= expect_u32(
        "USB0 OHCI", "HcFmInterval", fm_interval, 0xa7722ed8u);
    passed &= expect_u32(
        "USB0 OHCI", "HcPeriodicStart", periodic_start, 0x00002a29u);
    passed &= expect_u32(
        "USB0 OHCI", "HcLSThreshold", ls_threshold, 0x00000620u);
    passed &= expect_u32(
        "USB0 OHCI", "operational HcControl", control_operational, 0x000000beu);
    passed &= expect_u32(
        "USB0 OHCI",
        "initial HcInterruptEnable",
        interrupt_enable_initial,
        0x80000000u);
    passed &= expect_u32(
        "USB0 OHCI",
        "first HcInterruptEnable",
        interrupt_enable_first,
        0x80000033u);
    passed &= expect_u32(
        "USB0 OHCI", "reset HcRhStatus", rh_status_reset, 0u);
    passed &= expect_u32(
        "USB0 OHCI", "post-write HcRhStatus", rh_status_reset_after, 0u);
    for (size_t i = 0u; i < ARRAY_SIZE(root_port_status); ++i) {
        passed &= expect_u32(
            "USB0 OHCI", "root-port reset state", root_port_status[i], 0x100u);
        passed &= expect_u32(
            "USB0 OHCI",
            "root-port zero-write state",
            root_port_status_after[i],
            0x100u);
    }
    passed &= expect_u32(
        "USB0 OHCI",
        "second HcInterruptEnable",
        interrupt_enable_second,
        0x80000073u);
    passed &= expect_size(
        "USB0 OHCI",
        "access_count",
        recomp_runtime.access_count,
        ARRAY_SIZE(expected_addresses));
    passed &= expect_size(
        "USB0 OHCI",
        "undeclared_access_count",
        recomp_runtime.undeclared_access_count,
        0u);
    if (recomp_runtime.access_count == ARRAY_SIZE(expected_addresses)) {
        for (size_t i = 0; i < ARRAY_SIZE(expected_addresses); ++i) {
            passed &= expect_u32(
                "USB0 OHCI",
                "access.address",
                accesses[i].address,
                expected_addresses[i]);
            passed &= expect_u32(
                "USB0 OHCI", "access.width", accesses[i].width, 4u);
        }
    }

    return passed;
}

/* One contiguous 64MiB allocation mirrors what the full runner maps, and is
   the only shape that arms the direct RAM path. */
static uint8_t *calloc_ram_fixture(void)
{
    uint8_t *ram = calloc(0x04000000u, 1u);

    if (ram == NULL) {
        fprintf(stderr, "direct RAM: fixture allocation failed\n");
        exit(EXIT_FAILURE);
    }
    fill_object_window(ram, 0u, 0x04000000u);
    return ram;
}

static const RecompMemoryRegion full_ram_region(uint8_t *ram)
{
    return (RecompMemoryRegion){
        .address = 0u,
        .size = 0x04000000u,
        .data = ram,
    };
}

static int ram_byte_matches(
    const char *case_name,
    const uint8_t *memory,
    uint32_t address,
    size_t width)
{
    for (size_t i = 0; i < width; ++i) {
        uint8_t expected = (uint8_t)(((address + (uint32_t)i) * 33u + 17u) &
                                     0xffu);

        if (memory[i] != expected) {
            fprintf(
                stderr,
                "%s: byte at 0x%08x was 0x%02x, expected 0x%02x\n",
                case_name,
                address + (uint32_t)i,
                memory[i],
                expected);
            return 0;
        }
    }
    return 1;
}

static int run_direct_ram_windows(uint8_t *ram)
{
    static const size_t widths[] = {1u, 2u, 4u, 8u, 16u};
    static const struct {
        uint32_t base;
        const char *name;
    } windows[] = {
        {0x00000000u, "direct RAM"},
        {0x80000000u, "cached RAM alias"},
        {0xf0000000u, "physical RAM alias"},
    };
    RecompMemoryRegion region = full_ram_region(ram);
    int passed = 1;

    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);

    /* The u32 checks below must exercise the inline RAM path, which a
       RECOMP_WATCH debug session deliberately disables. */
    const char *watch = getenv("RECOMP_WATCH");
    if ((watch == NULL || watch[0] == '\0') && recomp_fast_ram != ram) {
        fprintf(stderr, "direct RAM: inline fast path not armed\n");
        passed = 0;
    }
    for (size_t w = 0; w < ARRAY_SIZE(widths); ++w) {
        size_t width = widths[w];
        uint32_t offset = 0x04000000u - (uint32_t)width;

        for (size_t v = 0; v < ARRAY_SIZE(windows); ++v) {
            uint32_t address = windows[v].base + offset;
            uint8_t *memory = recomp_memory(address, width);

            if (memory == NULL) {
                fprintf(
                    stderr,
                    "%s: %u-byte access at 0x%08x failed\n",
                    windows[v].name,
                    (unsigned)width,
                    address);
                passed = 0;
                continue;
            }
            passed &= ram_byte_matches(
                windows[v].name, memory, offset, width);
            passed &= ram_byte_matches(windows[v].name,
                recomp_memory(windows[v].base, width), 0u, width);
            if (width == sizeof(uint32_t)) {
                passed &= expect_u32(windows[v].name, "u32 boundary read",
                    *recomp_memory_u32(address), *(uint32_t *)(void *)memory);
            } else if (width == sizeof(int8_t)) {
                passed &= ram_byte_matches(windows[v].name,
                    (uint8_t *)(void *)recomp_memory_i8(address), offset, width);
            } else if (width == sizeof(uint16_t)) {
                passed &= ram_byte_matches(windows[v].name,
                    (uint8_t *)(void *)recomp_memory_u16(address), offset, width);
            } else if (width == sizeof(uint64_t)) {
                passed &= ram_byte_matches(windows[v].name,
                    (uint8_t *)(void *)recomp_memory_u64(address), offset, width);
            }
        }
    }

    /* A write through one window must be visible through the others. */
    *(uint32_t *)(void *)recomp_memory(0x81000000u, sizeof(uint32_t)) =
        0xc0ffee11u;
    passed &= expect_u32(
        "direct RAM",
        "cached write seen from direct window",
        *(uint32_t *)(void *)recomp_memory(0x01000000u, sizeof(uint32_t)),
        0xc0ffee11u);
    passed &= expect_u32(
        "direct RAM",
        "cached write seen from physical window",
        *(uint32_t *)(void *)recomp_memory(0xf1000000u, sizeof(uint32_t)),
        0xc0ffee11u);
    return passed;
}

static int run_alias_region_precedence(uint8_t *ram)
{
    uint32_t alias_words[] = {0xdeadbeefu, 0x0badf00du};
    RecompMemoryRegion regions[] = {
        full_ram_region(ram),
        {
            .address = 0x81000800u,
            .size = sizeof(alias_words),
            .data = (uint8_t *)alias_words,
        },
    };
    int passed = 1;

    recomp_runtime_init(
        regions, ARRAY_SIZE(regions), NULL, 0u, NULL, 0u);
    passed &= expect_u32(
        "alias precedence",
        "explicit region read",
        *(uint32_t *)(void *)recomp_memory(0x81000804u, sizeof(uint32_t)),
        0x0badf00du);
    *(uint32_t *)(void *)recomp_memory(0x81000800u, sizeof(uint32_t)) =
        0xfeedfaceu;
    passed &= expect_u32(
        "alias precedence", "explicit region write", alias_words[0],
        0xfeedfaceu);
    passed &= expect_u32(
        "alias precedence",
        "ram alias resolves to full region",
        *(uint32_t *)(void *)recomp_memory(0x81c7f7e4u, sizeof(uint32_t)),
        ((0x01c7f7e4u * 33u + 17u) & 0xffu) |
            (((0x01c7f7e5u * 33u + 17u) & 0xffu) << 8) |
            (((0x01c7f7e6u * 33u + 17u) & 0xffu) << 16) |
            (((0x01c7f7e7u * 33u + 17u) & 0xffu) << 24));
    passed &= expect_u32(
        "alias precedence",
        "direct window resolves to full region",
        *(uint32_t *)(void *)recomp_memory(0x01000004u, sizeof(uint32_t)),
        ((0x01000004u * 33u + 17u) & 0xffu) |
            (((0x01000005u * 33u + 17u) & 0xffu) << 8) |
            (((0x01000006u * 33u + 17u) & 0xffu) << 16) |
            (((0x01000007u * 33u + 17u) & 0xffu) << 24));
    return passed;
}

static int run_ram_access_logging(uint8_t *ram)
{
    RecompMemoryAccess accesses[4];
    RecompMemoryRegion region = full_ram_region(ram);
    int passed = 1;

    recomp_runtime_init(
        &region, 1u, accesses, ARRAY_SIZE(accesses), NULL, 0u);
    (void)*recomp_memory_u32(0x01c7f7e4u);
    *(uint32_t *)(void *)recomp_memory_u32(0x81c7f7e4u) = 0x11223344u;
    (void)*recomp_memory_u32(0xf368c000u);
    passed &= expect_size(
        "RAM logging", "access_count", recomp_runtime.access_count, 3u);
    if (recomp_runtime.access_count == 3u) {
        passed &= expect_u32(
            "RAM logging", "direct logged address",
            accesses[0].address, 0x01c7f7e4u);
        passed &= expect_u32(
            "RAM logging", "cached logged address",
            accesses[1].address, 0x81c7f7e4u);
        passed &= expect_u32(
            "RAM logging", "physical logged address",
            accesses[2].address, 0xf368c000u);
        for (size_t i = 0; i < 3u; ++i) {
            passed &= expect_u32(
                "RAM logging", "logged width", accesses[i].width, 4u);
        }
    }
    passed &= expect_u32(
        "RAM logging", "aliased write reached ram",
        *(uint32_t *)(void *)recomp_memory(0x01c7f7e4u, sizeof(uint32_t)),
        0x11223344u);
    return passed;
}

static int run_reinit_clears_direct_cache(uint8_t *ram)
{
    uint32_t small_words[] = {0u, 0u};
    RecompMemoryRegion small = {
        .address = 0x01c7f7e0u,
        .size = sizeof(small_words),
        .data = (uint8_t *)small_words,
    };
    int passed = 1;

    /* Arming the direct path first leaves a stale cache if reinit does not
       reset it; the small region must then win for its own span. */
    {
        RecompMemoryRegion region = full_ram_region(ram);

        recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    }
    recomp_runtime_init(&small, 1u, NULL, 0u, NULL, 0u);
    *(uint32_t *)(void *)recomp_memory_u32(0x81c7f7e4u) = 0x5a5a5a5au;
    passed &= expect_u32(
        "reinit cache", "small region write", small_words[1], 0x5a5a5a5au);
    return passed;
}

static int run_ram_overrun(void)
{
    uint8_t *ram = calloc_ram_fixture();
    RecompMemoryRegion region = full_ram_region(ram);

    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    (void)*recomp_memory_u32(0x83ffffffu);
    return EXIT_SUCCESS;
}

/* Location 0 allows entries 0, 2 and 3; 2 is playing, so only 0 and 3. */
static int run_music_shuffle_pick(void)
{
    const uint16_t masks[4] = {0x1u, 0x2u, 0x1u, 0x1u};
    int seen[4] = {0};

    for (uint32_t random = 0u; random < 16u; ++random) {
        int pick = recomp_music_shuffle_pick(masks, 4, 2, 0, random);

        if (pick != 0 && pick != 3) {
            fprintf(stderr, "music shuffle: picked %d\n", pick);
            return 0;
        }
        ++seen[pick];
    }
    return seen[0] > 0 && seen[3] > 0 &&
        recomp_music_shuffle_pick(masks, 4, 2, -1, 1u) == 1 &&
        recomp_music_shuffle_pick(masks, 1, 0, -1, 0u) == -1 &&
        recomp_music_shuffle_pick(masks, 2, 0, 1, 0u) == 1 &&
        recomp_music_shuffle_pick(masks, 2, 1, 1, 0u) == -1;
}

static int run_ram_pending_ohci(void)
{
    uint8_t *ram = calloc_ram_fixture();
    RecompMemoryRegion region = full_ram_region(ram);

    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    *recomp_memory_u32(0xfed00004u) = 0xffffffffu;
    (void)*recomp_memory_u32(0x00001000u);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    int passed = 1;

    if (argc == 2 && strcmp(argv[1], "--presenter-close-draw") == 0) {
        return recomp_d3d_draw_presenter_close_test(1);
    }
    if (argc == 2 && strcmp(argv[1], "--presenter-close-up") == 0) {
        return recomp_d3d_draw_presenter_close_test(0);
    }
    if (argc == 2 && strcmp(argv[1], "--presenter-close-release") == 0) {
        return recomp_kernel_presenter_close_test();
    }
    if (argc == 2 && strcmp(argv[1], "--custom-music") == 0) {
        return recomp_custom_music_test();
    }
    if (argc == 2 && strcmp(argv[1], "--invalid-access") == 0) {
        return run_invalid_access();
    }
    if (argc == 2 && strcmp(argv[1], "--fiber-stack-recycling") == 0) {
        return recomp_fiber_adapter_test() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && strcmp(argv[1], "--ram-overrun") == 0) {
        return run_ram_overrun();
    }
    if (argc == 2 && strcmp(argv[1], "--ram-pending-ohci") == 0) {
        return run_ram_pending_ohci();
    }
    if (argc != 1) {
        fprintf(
            stderr,
            "usage: recomp_runtime_test "
            "[--invalid-access|--fiber-stack-recycling|"
            "--ram-overrun|--ram-pending-ohci|--custom-music|"
            "--presenter-close-release|--presenter-close-draw|--presenter-close-up]\n");
        return 64;
    }

    for (size_t i = 0; i < ARRAY_SIZE(fixtures); ++i) {
        passed &= run_fixture(&fixtures[i]);
    }
    {
        uint8_t *ram = calloc_ram_fixture();

        passed &= run_direct_ram_windows(ram);
        passed &= run_alias_region_precedence(ram);
        passed &= run_ram_access_logging(ram);
        passed &= run_reinit_clears_direct_cache(ram);
        recomp_runtime_init(NULL, 0u, NULL, 0u, NULL, 0u);
        free(ram);
    }
    passed &= run_cached_ram_alias();
    passed &= run_music_shuffle_pick();
    passed &= run_physical_ram_alias();
    passed &= recomp_xapi_time_adapter_test();
    passed &= recomp_device_model_test();
    passed &= recomp_d3d_creation_model_test();
    passed &= recomp_d3d_draw_model_test();
    passed &= recomp_d3d_draw_adapter_test();
    passed &= recomp_d3d_frame_model_test();
    passed &= recomp_d3d_presenter_memory_test();
    passed &= recomp_d3d_frame_adapter_test();
    passed &= recomp_d3d_render_state_model_test();
    passed &= recomp_d3d_texture_model_test();
    passed &= recomp_d3d_tile_model_test();
    passed &= recomp_d3d_vertex_shader_model_test();
    passed &= recomp_dsound_service_adapter_test();
    passed &= recomp_cri_service_model_test();
    passed &= recomp_cri_service_adapter_test();
    passed &= recomp_crt_format_adapter_test();
    passed &= recomp_crt_string_adapter_test();
    passed &= recomp_animation_track_adapter_test();
    passed &= recomp_fiber_model_test();
    passed &= recomp_flag_macro_test();
    passed &= recomp_sse_semantics_test();
    passed &= recomp_directory_model_test();
    passed &= recomp_input_model_test();
    passed &= recomp_input_pulse_source_test();
    passed &= recomp_symbolic_link_model_test();
    passed &= recomp_ohci_model_test();
    passed &= recomp_apu_model_test();
    passed &= recomp_kernel_memory_test();
    passed &= recomp_kernel_file_save_test();
    passed &= recomp_kernel_thread_test();
    passed &= recomp_kernel_video_test();
    passed &= recomp_kernel_rtl_test();
    passed &= recomp_kernel_crypto_test();
    passed &= run_usb0_ohci_initialization();
    passed &= recomp_kernel_allocation_test();

    if (!passed) {
        return EXIT_FAILURE;
    }

#ifdef RECOMP_PUBLIC_LEAF_FIXTURE
    puts("recomp runtime: public leaf fixture and model tests passed");
#else
    puts("recomp runtime: all four banked sub_00189800 cases passed");
#endif
    return EXIT_SUCCESS;
}
