/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The runtime's state for SyncTest and rollback snapshots. Adapted from
   doa2u-re's synctest_modules.c for this runtime's modules. */
#include "synctest.h"

#include "fiber_adapter.h"
#include "d3d_frame_adapter.h"
#include "dsound_service_adapter.h"
#include "game_scheduler.h"
#include "input_adapter.h"
#include "kernel_abi.h"
#include "runtime.h"
#include "save_transaction.h"
#include "xapi_time_adapter.h"
#include "xbox_memory_layout.h"

#include <string.h>

/* Fiber stacks are the only ignored ranges, and the rollback hash takes them all. */
_Static_assert(RECOMP_FIBER_MAX_COUNT <= RECOMP_SNAPSHOT_HASH_RANGES, "ignored ranges fit the hash");

size_t recomp_synctest_ignored_ranges(RecompRamRange *ranges)
{
    /* Parked fibers' guest stacks below their saved esp are dead scratch;
       the snapshot still restores them. */
    RecompFiberStackRange stacks[RECOMP_FIBER_MAX_COUNT];
    const size_t count = recomp_fiber_adapter_stack_ranges(stacks, RECOMP_FIBER_MAX_COUNT);
    for (size_t i = 0u; i < count; ++i)
        ranges[i] = (RecompRamRange){stacks[i].base, (size_t)stacks[i].base + stacks[i].size};
    for (size_t i = 1u; i < count; ++i) {
        RecompRamRange value = ranges[i];
        size_t j = i;
        while (j > 0u && ranges[j - 1u].start > value.start) {
            ranges[j] = ranges[j - 1u];
            --j;
        }
        ranges[j] = value;
    }
    return count;
}

static bool ram_equal(const void *left, const void *right, size_t size)
{
    const uint8_t *a = left, *b = right;
    RecompRamRange ranges[RECOMP_FIBER_MAX_COUNT];
    const size_t range_count = recomp_synctest_ignored_ranges(ranges);
    size_t at = 0u;
    for (size_t range = 0u; range <= range_count && at < size; ++range) {
        const size_t end = range < range_count && ranges[range].start < size ? ranges[range].start : size;
        if (end > at && memcmp(a + at, b + at, end - at) != 0) return false;
        if (range < range_count && ranges[range].stop > at) at = ranges[range].stop;
    }
    return true;
}

void recomp_synctest_mask_ignored(uint8_t *ram, size_t size)
{
    RecompRamRange ranges[RECOMP_FIBER_MAX_COUNT];
    const size_t range_count = recomp_synctest_ignored_ranges(ranges);
    for (size_t i = 0u; i < range_count; ++i) {
        if (ranges[i].start >= size) continue;
        const size_t stop = ranges[i].stop < size ? ranges[i].stop : size;
        memset(ram + ranges[i].start, 0, stop - ranges[i].start);
    }
}

/* At the frame boundary only esp is defined; the rest is caller scratch. */
static bool cpu_equal(const void *left, const void *right, size_t size)
{
    const RecompRuntimeState *a = left, *b = right;
    (void)size;
    return a->registers.esp == b->registers.esp;
}
static bool fibers_restorable(const void *state, size_t size)
{
    (void)size;
    return recomp_fiber_adapter_state_restorable(state);
}
static bool files_restorable(const void *state, size_t size)
{
    (void)size;
    return recomp_kernel_file_state_restorable(state);
}
/* Save storage and its journal live on the host disk, outside the snapshot.
   A replay after a save sees the files the first pass wrote, and a rewind
   into an open transaction would never commit. Neither frame is rewound. */
typedef struct SaveStorageState {
    bool pending;
    uint32_t mutations;
} SaveStorageState;
static void save_storage_save(void *state)
{
    *(SaveStorageState *)state = (SaveStorageState){
        recomp_save_pending(), recomp_kernel_profile_mutations()};
}
static void save_storage_restore(const void *state) { (void)state; }
static bool save_storage_restorable(const void *state, size_t size)
{
    const SaveStorageState *saved = state;
    (void)size;
    return !saved->pending && saved->mutations == recomp_kernel_profile_mutations();
}

void recomp_synctest_install_default_modules(void)
{
    RecompSyncTestModule table[16];
    size_t count = 0u;
    recomp_synctest_set_ram_equal(ram_equal);
    table[count++] = (RecompSyncTestModule){"cpu", recomp_runtime_state_size(),
        recomp_runtime_state_save, recomp_runtime_state_restore, cpu_equal};
    table[count++] = (RecompSyncTestModule){"clock", recomp_xapi_frame_clock_state_size(),
        recomp_xapi_frame_clock_state_save, recomp_xapi_frame_clock_state_restore, NULL, NULL, true};
    table[count++] = (RecompSyncTestModule){"fibers", recomp_fiber_adapter_state_size(),
        recomp_fiber_adapter_state_save, recomp_fiber_adapter_state_restore,
        recomp_fiber_adapter_state_equal, fibers_restorable};
    table[count++] = (RecompSyncTestModule){"scheduler", recomp_game_scheduler_state_size(),
        recomp_game_scheduler_state_save, recomp_game_scheduler_state_restore};
    table[count++] = (RecompSyncTestModule){"threads", recomp_kernel_thread_state_size(),
        recomp_kernel_thread_state_save, recomp_kernel_thread_state_restore};
    table[count++] = (RecompSyncTestModule){"interrupts", recomp_kernel_interrupt_state_size(),
        recomp_kernel_interrupt_state_save, recomp_kernel_interrupt_state_restore};
    table[count++] = (RecompSyncTestModule){"files", recomp_kernel_file_state_size(),
        recomp_kernel_file_state_save, recomp_kernel_file_state_restore, NULL, files_restorable};
    table[count++] = (RecompSyncTestModule){"allocations", recomp_kernel_memory_state_size(),
        recomp_kernel_memory_state_save, recomp_kernel_memory_state_restore};
    table[count++] = (RecompSyncTestModule){"heap", xbox_HeapStateSize(),
        xbox_HeapStateSave, xbox_HeapStateRestore};
    table[count++] = (RecompSyncTestModule){"input", recomp_input_adapter_state_size(),
        recomp_input_adapter_state_save, recomp_input_adapter_state_restore};
    table[count++] = (RecompSyncTestModule){"d3d_frame", recomp_d3d_frame_adapter_state_size(),
        recomp_d3d_frame_adapter_state_save, recomp_d3d_frame_adapter_state_restore};
    table[count++] = (RecompSyncTestModule){"dsound", recomp_dsound_service_adapter_state_size(),
        recomp_dsound_service_adapter_state_save, recomp_dsound_service_adapter_state_restore};
    table[count++] = (RecompSyncTestModule){"save_storage", sizeof(SaveStorageState),
        save_storage_save, save_storage_restore, NULL, save_storage_restorable, true};
    recomp_synctest_set_modules(table, count);
}
