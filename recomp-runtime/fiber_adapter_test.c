#include "fiber_adapter.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TEST_TLS_INDEX_ADDRESS = 0x003b5258u,
    TEST_HEAP_BASE = 0x27000000u,
    TEST_HEAP_SIZE = 0x00200000u,
    TEST_STATE_BASE = 0x28000000u,
    TEST_STATE_SIZE = 0x00002000u,
    TEST_TLS_BLOCK = TEST_STATE_BASE + 0x100u,
    TEST_ENTRY_ESP = TEST_STATE_BASE + 0x1100u,
    TEST_STACK_SIZE = 0x3000u,
    TEST_FIBER_ENTRY = 0x000b5570u,
};

void recomp_test_heap_reset(uint32_t cursor, int fail_after);

static int expect_u32(const char *field, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(
        stderr,
        "fiber adapter: %s was 0x%08x, expected 0x%08x\n",
        field,
        actual,
        expected);
    return 0;
}

static void prepare_call(uint32_t argument_count, const uint32_t *arguments)
{
    uint32_t *stack = recomp_memory_u32(TEST_ENTRY_ESP);

    stack[0] = 0x0010abcdu;
    for (uint32_t i = 0u; i < argument_count; ++i) {
        stack[i + 1u] = arguments[i];
    }
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
}

static const RecompFiber *find_fiber(uint32_t guest_handle)
{
    const RecompFiberModel *model = recomp_fiber_adapter_model();

    for (size_t i = 0u; i < RECOMP_FIBER_MAX_COUNT; ++i) {
        const RecompFiber *fiber = &model->fibers[i];

        if (fiber->active && fiber->guest_handle == guest_handle) {
            return fiber;
        }
    }
    return NULL;
}

#ifdef _WIN32
static uint32_t test_main_handle;
static uint32_t test_fiber_visits;

static void switch_to(uint32_t guest_handle)
{
    prepare_call(1u, &guest_handle);
    recomp_fiber_lookup_manual(0x0018305au)();
}

/* Counts on its own native stack, so only a rewound stack can repeat a count. */
static void test_fiber_body(void)
{
    volatile uint32_t visits = 0u;

    for (;;) {
        test_fiber_visits = ++visits;
        switch_to(test_main_handle);
    }
}

static RecompFunction test_fiber_lookup(uint32_t guest_address)
{
    return guest_address == TEST_FIBER_ENTRY ? test_fiber_body : NULL;
}

/* A snapshot of a parked fiber rewinds its native continuation. */
static int parked_fiber_rewinds(void *saved)
{
    uint32_t arguments[3] = {0xabcdef01u};
    uint32_t fiber_handle;
    int passed = 1;

    recomp_runtime_set_lookup(test_fiber_lookup);
    prepare_call(1u, arguments);
    recomp_fiber_lookup_manual(0x00183099u)();
    test_main_handle = recomp_runtime.registers.eax;
    arguments[0] = TEST_STACK_SIZE;
    arguments[1] = TEST_FIBER_ENTRY;
    arguments[2] = 0u;
    prepare_call(3u, arguments);
    recomp_fiber_lookup_manual(0x00182fbbu)();
    fiber_handle = recomp_runtime.registers.eax;

    switch_to(fiber_handle);
    recomp_fiber_adapter_state_save(saved);
    passed &= expect_u32("parked fiber restorable", recomp_fiber_adapter_state_restorable(saved), 1u);
    switch_to(fiber_handle);
    switch_to(fiber_handle);
    passed &= expect_u32("fiber visits before rewind", test_fiber_visits, 3u);
    recomp_fiber_adapter_state_restore(saved);
    switch_to(fiber_handle);
    passed &= expect_u32("fiber visits after rewind", test_fiber_visits, 2u);

    recomp_fiber_adapter_reset();
    recomp_runtime_set_lookup(NULL);
    return passed;
}
#endif

int recomp_fiber_adapter_test(void)
{
    static uint8_t low_memory[8];
    static uint8_t tls_index_memory[4];
    static uint8_t heap_memory[TEST_HEAP_SIZE];
    static uint8_t state_memory[TEST_STATE_SIZE];
    const RecompMemoryRegion regions[] = {
        {
            .address = 0u,
            .size = sizeof low_memory,
            .data = low_memory,
        },
        {
            .address = TEST_TLS_INDEX_ADDRESS,
            .size = sizeof tls_index_memory,
            .data = tls_index_memory,
        },
        {
            .address = TEST_HEAP_BASE,
            .size = sizeof heap_memory,
            .data = heap_memory,
        },
        {
            .address = TEST_STATE_BASE,
            .size = sizeof state_memory,
            .data = state_memory,
        },
    };
    const uint32_t first_parameter = 0x11111111u;
    const uint32_t second_parameter = 0x22222222u;
    uint32_t arguments[3];
    uint32_t first_handle;
    uint32_t first_heap_checkpoint;
    uint32_t second_handle;
    RecompFunction adapter;
    const RecompFiber *fiber;
    RecompFiberStackRange range = {0};
    void *saved = malloc(recomp_fiber_adapter_state_size());
    void *empty = malloc(recomp_fiber_adapter_state_size());
    int passed = 1;

    if (saved == NULL || empty == NULL) {
        free(saved);
        free(empty);
        return 0;
    }
    memset(low_memory, 0, sizeof low_memory);
    memset(tls_index_memory, 0, sizeof tls_index_memory);
    memset(heap_memory, 0xa5, sizeof heap_memory);
    memset(state_memory, 0, sizeof state_memory);
    recomp_runtime_init(regions, 4u, NULL, 0u, NULL, 0u);
    recomp_test_heap_reset(TEST_HEAP_BASE, -1);
    recomp_fiber_adapter_reset();

    *recomp_memory_u32(0u) = 0xffffffffu;
    *recomp_memory_u32(4u) = TEST_STATE_BASE;
    *recomp_memory_u32(TEST_TLS_INDEX_ADDRESS) = 0u;
    *recomp_memory_u32(TEST_STATE_BASE) = TEST_TLS_BLOCK;

    arguments[0] = 0xabcdef01u;
    prepare_call(1u, arguments);
    adapter = recomp_fiber_lookup_manual(0x00183099u);
    adapter();
    recomp_fiber_adapter_state_save(empty);

    arguments[0] = TEST_STACK_SIZE;
    arguments[1] = TEST_FIBER_ENTRY;
    arguments[2] = first_parameter;
    prepare_call(3u, arguments);
    adapter = recomp_fiber_lookup_manual(0x00182fbbu);
    adapter();
    first_handle = recomp_runtime.registers.eax;
    first_heap_checkpoint = xbox_HeapCheckpoint();
    recomp_fiber_adapter_state_save(saved);

    arguments[0] = first_handle;
    prepare_call(1u, arguments);
    adapter = recomp_fiber_lookup_manual(0x00183047u);
    adapter();
    passed &= expect_u32(
        "deleted fiber absent",
        find_fiber(first_handle) != NULL,
        0u);
    *recomp_memory_u32(TEST_HEAP_BASE + 0x100u) = 0xdeadbeefu;

    arguments[0] = TEST_STACK_SIZE;
    arguments[1] = TEST_FIBER_ENTRY;
    arguments[2] = second_parameter;
    prepare_call(3u, arguments);
    adapter = recomp_fiber_lookup_manual(0x00182fbbu);
    adapter();
    second_handle = recomp_runtime.registers.eax;

    passed &= expect_u32(
        "guest stack handle reused", second_handle, first_handle);
    passed &= expect_u32(
        "guest heap unchanged",
        xbox_HeapCheckpoint(),
        first_heap_checkpoint);
    passed &= expect_u32(
        "guest stack base",
        *recomp_memory_u32(second_handle + 8u),
        TEST_HEAP_BASE);
    passed &= expect_u32(
        "guest stack parameter",
        *recomp_memory_u32(second_handle),
        second_parameter);
    passed &= expect_u32(
        "guest stack cleared",
        *recomp_memory_u32(TEST_HEAP_BASE + 0x100u),
        0u);
    fiber = find_fiber(second_handle);
    passed &= expect_u32("recreated fiber present", fiber != NULL, 1u);
    if (fiber != NULL) {
        passed &= expect_u32(
            "recreated fiber parameter", fiber->parameter, second_parameter);
    }

    /* Rewinding the delete and the re-create puts the first fiber back in its slot. */
    recomp_fiber_adapter_state_restore(saved);
    fiber = find_fiber(first_handle);
    passed &= expect_u32(
        "restored fiber parameter",
        fiber != NULL ? fiber->parameter : 0u,
        first_parameter);
    passed &= expect_u32(
        "restored stack count", (uint32_t)recomp_fiber_adapter_stack_ranges(&range, 1u), 1u);
    passed &= expect_u32("restored stack base", range.base, TEST_HEAP_BASE);
    passed &= expect_u32("restored stack size", range.size, TEST_STACK_SIZE);

    /* Rewinding past the create frees the slot but keeps its guest stack. */
    recomp_fiber_adapter_state_restore(empty);
    passed &= expect_u32("rewound fiber absent", find_fiber(first_handle) != NULL, 0u);
    arguments[0] = TEST_STACK_SIZE;
    arguments[1] = TEST_FIBER_ENTRY;
    arguments[2] = second_parameter;
    prepare_call(3u, arguments);
    adapter = recomp_fiber_lookup_manual(0x00182fbbu);
    adapter();
    passed &= expect_u32("rewound stack reused", recomp_runtime.registers.eax, first_handle);
    passed &= expect_u32(
        "rewound create heap unchanged", xbox_HeapCheckpoint(), first_heap_checkpoint);

    arguments[0] = second_handle;
    prepare_call(1u, arguments);
    adapter = recomp_fiber_lookup_manual(0x00183047u);
    adapter();
    recomp_fiber_adapter_reset();
#ifdef _WIN32
    passed &= parked_fiber_rewinds(saved);
#endif
    free(saved);
    free(empty);
    return passed;
}
