#include "device_model.h"
#include "runtime.h"
#include "xbox_memory_layout.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    TEST_HEAP_BASE = 0x27000000u,
    TEST_HEAP_SIZE = 0x200000u,
    STATUS_SUCCESS = 0x00000000u,
    STATUS_NO_MEMORY = 0xc0000017u,
};

static uint32_t test_heap_cursor;
static int test_heap_fail_after;
static unsigned test_heap_allocations;
static uint32_t test_heap_freed;
static unsigned test_contiguous_allocations;
static uint32_t test_contiguous_size;
static uint32_t test_contiguous_lowest;
static uint32_t test_contiguous_highest;
static uint32_t test_contiguous_alignment;

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t aligned;

    if (test_heap_fail_after >= 0 &&
        test_heap_allocations >= (unsigned)test_heap_fail_after) {
        return 0u;
    }
    aligned = (test_heap_cursor + alignment - 1u) & ~(alignment - 1u);
    if ((uint64_t)aligned + size > TEST_HEAP_BASE + TEST_HEAP_SIZE) {
        return 0u;
    }
    test_heap_cursor = aligned + size;
    ++test_heap_allocations;
    return aligned;
}

uint32_t xbox_ContiguousAlloc(
    uint32_t size,
    uint32_t lowest_address,
    uint32_t highest_address,
    uint32_t alignment)
{
    ++test_contiguous_allocations;
    test_contiguous_size = size;
    test_contiguous_lowest = lowest_address;
    test_contiguous_highest = highest_address;
    test_contiguous_alignment = alignment;
    if (alignment < 0x1000u) {
        alignment = 0x1000u;
    }
    return xbox_HeapAlloc(size, alignment);
}

unsigned recomp_test_contiguous_allocation_count(void)
{
    return test_contiguous_allocations;
}

void recomp_test_contiguous_allocation_arguments(
    uint32_t *size,
    uint32_t *lowest_address,
    uint32_t *highest_address,
    uint32_t *alignment)
{
    *size = test_contiguous_size;
    *lowest_address = test_contiguous_lowest;
    *highest_address = test_contiguous_highest;
    *alignment = test_contiguous_alignment;
}

void xbox_HeapFree(uint32_t guest_address)
{
    test_heap_freed = guest_address;
}

uint32_t xbox_HeapCheckpoint(void)
{
    return test_heap_cursor;
}

bool xbox_HeapRestore(uint32_t checkpoint)
{
    if (checkpoint < TEST_HEAP_BASE || checkpoint > test_heap_cursor) {
        return false;
    }
    test_heap_cursor = checkpoint;
    return true;
}

/* The cursor and the allocation count that fail_after reads. */
size_t xbox_HeapStateSize(void) { return 2u * sizeof(uint32_t); }

void xbox_HeapStateSave(void *state)
{
    const uint32_t saved[2] = {test_heap_cursor, test_heap_allocations};
    memcpy(state, saved, sizeof saved);
}

void xbox_HeapStateRestore(const void *state)
{
    uint32_t saved[2];
    memcpy(saved, state, sizeof saved);
    test_heap_cursor = saved[0];
    test_heap_allocations = saved[1];
}

static int expect_u32(const char *field, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(
        stderr,
        "device model: %s was 0x%08x, expected 0x%08x\n",
        field,
        actual,
        expected);
    return 0;
}

void recomp_test_heap_reset(uint32_t cursor, int fail_after)
{
    test_heap_cursor = cursor;
    test_heap_fail_after = fail_after;
    test_heap_allocations = 0u;
    test_heap_freed = 0u;
    test_contiguous_allocations = 0u;
    test_contiguous_size = 0u;
    test_contiguous_lowest = 0u;
    test_contiguous_highest = 0u;
    test_contiguous_alignment = 0u;
}

int recomp_device_model_test(void)
{
    static uint8_t heap[TEST_HEAP_SIZE];
    const RecompMemoryRegion region = {
        .address = TEST_HEAP_BASE,
        .size = sizeof heap,
        .data = heap,
    };
    const uint32_t output_pointer = TEST_HEAP_BASE + 0x20u;
    const uint32_t object = TEST_HEAP_BASE + 0x100u;
    const uint32_t extension = object + 0x40u;
    RecompDeviceCreateResult result;
    int passed = 1;
    uint32_t disk[8];
    uint32_t written;
    memset(disk, 0xa5, sizeof disk);
    passed &= recomp_device_disk_query(0x70000u, disk, sizeof disk, &written) == 0u;
    passed &= written == 24u && disk[5] == 512u && disk[6] == 0xa5a5a5a5u;
    uint64_t capacity = (uint64_t)disk[0] * disk[3] * disk[4] * disk[5];
    passed &= recomp_device_disk_query(0x74004u, disk, sizeof disk, &written) == 0u;
    passed &= written == 32u && (((uint64_t)disk[3] << 32) | disk[2]) == capacity;
    memset(disk, 0xa5, sizeof disk);
    passed &= recomp_device_disk_query(0x74004u, disk, 31u, &written) == 0xc0000023u;
    passed &= written == 0u && disk[0] == 0xa5a5a5a5u;
    if (!passed) fprintf(stderr, "virtual disk geometry contract failed\n");

    memset(heap, 0xa5, sizeof heap);
    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    recomp_test_heap_reset(object, -1);
    result = recomp_device_create(0x170u, output_pointer);

    passed &= expect_u32("status", result.status, STATUS_SUCCESS);
    passed &= expect_u32("device_object", result.device_object, object);
    passed &= expect_u32(
        "device_extension", result.device_extension, extension);
    passed &= expect_u32(
        "published object", *recomp_memory_u32(output_pointer), object);
    passed &= expect_u32(
        "object extension", *recomp_memory_u32(object + 0x18u), extension);
    for (uint32_t offset = 0u; offset < 0x40u; ++offset) {
        if (offset >= 0x18u && offset < 0x1cu) {
            continue;
        }
        if ((uint8_t)*recomp_memory_i8(object + offset) != 0u) {
            fprintf(stderr, "device model: object byte 0x%x was not zero\n", offset);
            passed = 0;
            break;
        }
    }
    for (uint32_t offset = 0u; offset < 0x170u; ++offset) {
        if ((uint8_t)*recomp_memory_i8(extension + offset) != 0u) {
            fprintf(stderr, "device model: extension byte 0x%x was not zero\n", offset);
            passed = 0;
            break;
        }
    }

    *recomp_memory_u32(output_pointer) = 0xa5a5a5a5u;
    recomp_test_heap_reset(TEST_HEAP_BASE + 0x400u, 1);
    result = recomp_device_create(0x170u, output_pointer);
    passed &= expect_u32("failure status", result.status, STATUS_NO_MEMORY);
    passed &= expect_u32("failure object", result.device_object, 0u);
    passed &= expect_u32("failure extension", result.device_extension, 0u);
    passed &= expect_u32(
        "cleared output", *recomp_memory_u32(output_pointer), 0u);
    passed &= expect_u32(
        "freed partial object", test_heap_freed, TEST_HEAP_BASE + 0x400u);

    return passed;
}
