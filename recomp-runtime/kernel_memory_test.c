#include "kernel_abi.h"
#include "d3d_frame_adapter.h"
#include "d3d_presenter_memory_test.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <string.h>

enum {
    TEST_MEMORY_BASE = 0x2a000000u,
    TEST_MEMORY_SIZE = 0x1000u,
    TEST_ENTRY_ESP = TEST_MEMORY_BASE + 0x100u,
    TEST_SECTION = TEST_MEMORY_BASE + 0x200u,
    TEST_REFERENCE_OFFSET = 0x218u,
};

static int expect_u32(const char *field, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(stderr, "kernel memory: %s was 0x%08x, expected 0x%08x\n",
        field, actual, expected);
    return 0;
}

static int expect_resident(const uint8_t *memory, const uint8_t *before)
{
    return expect_u32("resident bytes before reference count",
        memcmp(memory, before, TEST_REFERENCE_OFFSET) == 0, 1u) &
        expect_u32("resident bytes after reference count",
            memcmp(memory + TEST_REFERENCE_OFFSET + 4u,
                before + TEST_REFERENCE_OFFSET + 4u,
                TEST_MEMORY_SIZE - TEST_REFERENCE_OFFSET - 4u) == 0, 1u);
}

int recomp_kernel_memory_test(void)
{
    static uint8_t memory[TEST_MEMORY_SIZE];
    static uint8_t before[TEST_MEMORY_SIZE];
    uint8_t zero_page[0x38u];
    uint8_t zero_before[sizeof zero_page];
    const RecompMemoryRegion regions[] = {
        {.address = TEST_MEMORY_BASE, .size = sizeof memory, .data = memory},
        {.address = 0u, .size = sizeof zero_page, .data = zero_page},
    };
    const uint32_t invalid = 0xc000000du;
    const uint32_t bad_sections[] = {
        0u,
        TEST_MEMORY_BASE - 0x38u,
        TEST_MEMORY_BASE + TEST_MEMORY_SIZE - 0x34u,
        0xffffffe0u,
    };
    const struct {
        uint32_t ordinal;
        uint32_t initial_count;
        uint32_t status;
        uint32_t final_count;
    } bridges[] = {
        {327u, 0u, 0u, 1u},
        {328u, 1u, 0u, 0u},
        {327u, UINT32_MAX, 0xc000000du, UINT32_MAX},
        {328u, 0u, 0xc000000du, 0u},
    };
    uint32_t *references;
    int passed = 1;

    memset(memory, 0xa5, sizeof memory);
    memset(zero_page, 0x5a, sizeof zero_page);
    recomp_runtime_init(regions, 1u, NULL, 0u, NULL, 0u);
    references = recomp_memory_u32(TEST_SECTION + 0x18u);
    *recomp_memory_u32(TEST_SECTION + 4u) = TEST_MEMORY_BASE + 0x400u;
    *recomp_memory_u32(TEST_SECTION + 8u) = 0x40u;
    *recomp_memory_u32(TEST_SECTION + 0x1cu) = TEST_MEMORY_BASE + 0x500u;
    *recomp_memory_u32(TEST_SECTION + 0x20u) = TEST_MEMORY_BASE + 0x502u;
    *references = 2u;
    memcpy(before, memory, sizeof before);

    for (uint32_t step = 0u; step < 2u; ++step) {
        passed &= expect_u32("balanced unload status",
            recomp_kernel_unload_section(TEST_SECTION), 0u);
        passed &= expect_u32("balanced unload count", *references, 1u - step);
        passed &= expect_resident(memory, before);
    }
    passed &= expect_u32("underflow status",
        recomp_kernel_unload_section(TEST_SECTION), invalid);
    passed &= expect_u32("underflow count unchanged", *references, 0u);
    passed &= expect_resident(memory, before);
    for (uint32_t count = 1u; count <= 2u; ++count) {
        passed &= expect_u32("resident reload status",
            recomp_kernel_load_section(TEST_SECTION), 0u);
        passed &= expect_u32("resident reload count", *references, count);
        passed &= expect_resident(memory, before);
    }
    *references = UINT32_MAX;
    passed &= expect_u32("overflow status",
        recomp_kernel_load_section(TEST_SECTION), invalid);
    passed &= expect_u32("overflow count unchanged", *references, UINT32_MAX);
    passed &= expect_resident(memory, before);

    /* The truncated header still contains +0x18: validating only the count
       would incorrectly accept it. A mapped address zero remains invalid. */
    for (size_t region_count = 1u; region_count <= 2u; ++region_count) {
        recomp_runtime_init(regions, region_count, NULL, 0u, NULL, 0u);
        memcpy(before, memory, sizeof before);
        memcpy(zero_before, zero_page, sizeof zero_before);
        for (size_t i = 0u; i < sizeof bad_sections / sizeof bad_sections[0]; ++i) {
            passed &= expect_u32("invalid section load",
                recomp_kernel_load_section(bad_sections[i]), invalid);
            passed &= expect_u32("invalid section unload",
                recomp_kernel_unload_section(bad_sections[i]), invalid);
            passed &= expect_u32("invalid section preserves memory",
                memcmp(memory, before, sizeof before) == 0, 1u);
            passed &= expect_u32("invalid section preserves zero page",
                memcmp(zero_page, zero_before, sizeof zero_before) == 0, 1u);
        }
    }

    for (size_t i = 0u; i < sizeof bridges / sizeof bridges[0]; ++i) {
        RecompFunction bridge = recomp_kernel_memory(bridges[i].ordinal);
        passed &= expect_u32("section ordinal registered", bridge != NULL, 1u);
        if (bridge == NULL) {
            continue;
        }
        *references = bridges[i].initial_count;
        *recomp_memory_u32(TEST_ENTRY_ESP) = 0x0010abcdu;
        *recomp_memory_u32(TEST_ENTRY_ESP + 4u) = TEST_SECTION;
        recomp_runtime.registers.esp = TEST_ENTRY_ESP;
        recomp_runtime.registers.eax = 0xccccccccu;
        memcpy(before, memory, sizeof before);
        bridge();
        passed &= expect_u32("section bridge EAX",
            recomp_runtime.registers.eax, bridges[i].status);
        passed &= expect_u32("section bridge ESP",
            recomp_runtime.registers.esp, TEST_ENTRY_ESP + 8u);
        passed &= expect_u32("section bridge count",
            *references, bridges[i].final_count);
        passed &= expect_resident(memory, before);
    }
    return passed;
}

void recomp_test_heap_reset(uint32_t cursor, int fail_after);

static uint32_t memory_call(uint32_t ordinal, const uint32_t *args, size_t count)
{
    *recomp_memory_u32(TEST_ENTRY_ESP) = 0u;
    for (size_t i = 0; i < count; ++i) {
        *recomp_memory_u32(TEST_ENTRY_ESP + 4u + (uint32_t)i * 4u) = args[i];
    }
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    recomp_kernel_memory(ordinal)();
    return recomp_runtime.registers.eax;
}

int recomp_kernel_allocation_test(void)
{
    enum { HEAP = 0x27000000u };
    static uint8_t heap[0x20000u], stack[TEST_MEMORY_SIZE];
    const RecompMemoryRegion regions[] = {
        {.address = HEAP, .size = sizeof heap, .data = heap},
        {.address = TEST_MEMORY_BASE, .size = sizeof stack, .data = stack},
    };
    int passed = 1;
    recomp_runtime_init(regions, 2u, NULL, 0u, NULL, 0u);
    recomp_test_heap_reset(HEAP + 0x1000u, -1);
    uint32_t block = recomp_kernel_allocate_pool(0x4000u);
    uint32_t live = recomp_kernel_allocate_pool(16u);
    memset(recomp_memory_u32(block), 0xa5, 0x4000u);
    *recomp_memory_u32(live) = 0x12345678u;
    recomp_kernel_free_pool(block);

    /* Freed memory must remain usable when the backing arena is exhausted. */
    recomp_test_heap_reset(HEAP + 0x10000u, 0);
    uint32_t small = recomp_kernel_allocate_pool(0x1000u);
    passed &= expect_u32("reuse with exhausted arena", small, block);
    if (small == 0u) return 0;
    uint32_t tail = recomp_kernel_allocate_pool(0x3000u);
    passed &= expect_u32("split free remainder", tail, block + 0x1000u);
    if (tail == 0u) return 0;
    for (uint32_t i = 0; i < 0x4000u; ++i) {
        if (*recomp_memory_i8(block + i) != 0) {
            passed &= expect_u32("reused memory zeroed", 1u, 0u);
            break;
        }
    }
    recomp_kernel_free_pool(small);
    recomp_kernel_free_pool(tail);
    uint32_t merged = recomp_kernel_allocate_pool(0x4000u);
    passed &= expect_u32("adjacent frees coalesced", merged, block);
    recomp_kernel_free_pool(merged);
    for (unsigned i = 0; i < 4096u; ++i) {
        uint32_t recycled = recomp_kernel_allocate_pool(0x2000u);
        if (recycled != block) {
            passed &= expect_u32("repeated pool reuse", recycled, block);
            break;
        }
        recomp_kernel_free_pool(recycled);
    }
    passed &= expect_u32("live allocation preserved", *recomp_memory_u32(live), 0x12345678u);
    passed &= expect_u32("reuse did not grow arena", xbox_HeapCheckpoint(), HEAP + 0x10000u);

    /* Contiguous reuse must preserve physical bounds and page alignment. */
    recomp_test_heap_reset(HEAP + 0x10000u, -1);
    uint32_t request[] = {0x1800u, 0u, UINT32_MAX, 0x1000u, 4u};
    uint32_t contiguous = memory_call(166u, request, 5u);
    passed &= expect_u32("contiguous page alignment", contiguous & 0xfffu, 0u);
    if (contiguous == 0u) return 0;
    memory_call(171u, &contiguous, 1u);
    recomp_test_heap_reset(HEAP + 0x18000u, 0);
    request[0] = 0x800u;
    request[1] = contiguous;
    request[2] = contiguous + 0xfffu;
    uint32_t reused = memory_call(166u, request, 5u);
    passed &= expect_u32("bounded contiguous reuse", reused, contiguous);
    request[1] = contiguous + 0x1000u;
    request[2] = contiguous + 0x1fffu;
    passed &= expect_u32("contiguous remainder", memory_call(166u, request, 5u), contiguous + 0x1000u);
    request[1] = contiguous;
    request[2] = contiguous + 0xfffu;
    passed &= expect_u32("live contiguous unavailable", memory_call(166u, request, 5u), 0u);
    memory_call(171u, &reused, 1u);
    request[3] = 0x1800u;
    passed &= expect_u32("invalid alignment", memory_call(166u, request, 5u), 0u);
    request[3] = 0x1000u;
    request[2] = contiguous + 0x7ffu;
    passed &= expect_u32("rounded extent upper bound", memory_call(166u, request, 5u), 0u);
    return passed;
}

/* Runs in its own process because a closed presenter exits normally. */
int recomp_kernel_presenter_close_test(void)
{
    enum { HEAP = 0x27000000u };
    static uint8_t heap[0x20000u];
    const RecompMemoryRegion region = {
        .address = HEAP, .size = sizeof heap, .data = heap,
    };
    const RecompD3dPresenterConfig config = {
        4u, 4u, RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
        RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8,
    };
    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    recomp_test_heap_reset(HEAP + 0x1000u, -1);
    recomp_d3d_frame_adapter_initialize(&config, HEAP);
    uint32_t block = recomp_kernel_allocate_pool(16u);
    if (block == 0u) return 1;
    recomp_d3d_presenter_memory_set_error(RECOMP_D3D_PRESENTER_CLOSED);
    recomp_kernel_free_pool(block);
    return 1; /* The close must exit before returning here. */
}
