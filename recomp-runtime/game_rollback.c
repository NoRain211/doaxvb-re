#include "game_rollback.h"
#include "fiber_adapter.h"
#include "runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum { PAGE_BYTES = 4096 };
static uint8_t *ram, *shadow;
static size_t ram_bytes;
static void **pages;
static RecompFpuContext fpu;

/* Pages written since the previous call. */
static ULONG_PTR written(void)
{
    ULONG_PTR count = ram_bytes / PAGE_BYTES;
    DWORD granularity;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, ram, ram_bytes, pages, &count, &granularity) != 0) {
        recomp_stop(1, "game:rollback-write-watch");
    }
    return count;
}

bool recomp_game_rollback_begin(void)
{
    if (shadow != NULL) return true;
    if (recomp_runtime.memory_region_count != 1u) return false;
    ram = recomp_runtime.memory_regions[0].data;
    ram_bytes = recomp_runtime.memory_regions[0].size;
    pages = malloc(ram_bytes / PAGE_BYTES * sizeof *pages);
    ULONG_PTR count = ram_bytes / PAGE_BYTES;
    DWORD granularity;
    if (pages == NULL ||
        GetWriteWatch(WRITE_WATCH_FLAG_RESET, ram, ram_bytes, pages, &count, &granularity) != 0) {
        free(pages);
        pages = NULL;
        return false;
    }
    shadow = VirtualAlloc(NULL, ram_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (shadow == NULL) return false;
    memcpy(shadow, ram, ram_bytes);
    recomp_fiber_adapter_checkpoint();
    recomp_fpu_context_save(&fpu);
    return true;
}

void recomp_game_rollback_commit(void)
{
    const ULONG_PTR count = written();
    for (ULONG_PTR i = 0; i < count; ++i) {
        memcpy(shadow + ((uint8_t *)pages[i] - ram), pages[i], PAGE_BYTES);
    }
    recomp_fiber_adapter_checkpoint();
    recomp_fpu_context_save(&fpu);
}

void recomp_game_rollback_restore(void)
{
    const ULONG_PTR count = written();
    for (ULONG_PTR i = 0; i < count; ++i) {
        memcpy(pages[i], shadow + ((uint8_t *)pages[i] - ram), PAGE_BYTES);
    }
    ResetWriteWatch(ram, ram_bytes);
    recomp_fiber_adapter_rollback();
    recomp_fpu_context_restore(&fpu);
}
#else
bool recomp_game_rollback_begin(void) { return false; }
void recomp_game_rollback_commit(void) {}
void recomp_game_rollback_restore(void) {}
#endif
