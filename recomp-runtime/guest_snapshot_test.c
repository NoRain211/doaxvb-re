#include "guest_snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

enum { PAGE = RECOMP_SNAPSHOT_PAGE_SIZE, PAGES = 64, RAM_SIZE = PAGES * PAGE };

static uint8_t initial[RAM_SIZE];
static uint8_t first[RAM_SIZE];
static uint8_t second[RAM_SIZE];

static int fail(const char *mode, const char *what)
{
    fprintf(stderr, "guest snapshot (%s): %s\n", mode, what);
    return 1;
}

static void fill(uint8_t *ram, size_t page, uint8_t value)
{
    memset(ram + page * PAGE, value, PAGE);
}

/* What a fresh snapshot of ram hashes to, against which the incremental one is checked. */
static uint64_t fresh_hash(uint8_t *ram, const RecompRamRange *ignored, size_t count)
{
    RecompGuestSnapshots t;
    uint64_t hash = 0u;
    if (recomp_snapshots_init(&t, ram, RAM_SIZE, false, 1u)) (void)recomp_snapshots_hash(&t, ignored, count, &hash);
    recomp_snapshots_free(&t);
    return hash;
}

#ifdef _WIN32
/* Host read workers fill guest RAM with ReadFile; write watch must see it. */
static bool read_file_into(uint8_t *target)
{
    char directory[MAX_PATH];
    char path[MAX_PATH];
    uint8_t bytes[PAGE];
    DWORD done = 0u;
    if (GetTempPathA(MAX_PATH, directory) == 0u ||
        GetTempFileNameA(directory, "rgs", 0u, path) == 0u) {
        return false;
    }
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0u, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    memset(bytes, 0x5a, sizeof bytes);
    const bool ok = WriteFile(file, bytes, PAGE, &done, NULL) && done == PAGE &&
        SetFilePointer(file, 0, NULL, FILE_BEGIN) == 0u &&
        ReadFile(file, target, PAGE, &done, NULL) && done == PAGE && target[0] == 0x5au;
    CloseHandle(file);
    return ok;
}
#endif

static int run(uint8_t *ram, bool write_watch, const char *mode)
{
    RecompGuestSnapshots s;
    for (size_t i = 0u; i < RAM_SIZE; ++i) ram[i] = (uint8_t)(i * 7u);
    memcpy(initial, ram, RAM_SIZE);
    if (!recomp_snapshots_init(&s, ram, RAM_SIZE, write_watch, 2u)) return fail(mode, "init");

    ram[3 * PAGE + 5] = 0xaau;
    if (!recomp_snapshots_save(&s)) return fail(mode, "first save");
    memcpy(first, ram, RAM_SIZE);
    fill(ram, 3, 0xbb);
    fill(ram, 10, 0xcc);
    if (!recomp_snapshots_save(&s)) return fail(mode, "second save");
    memcpy(second, ram, RAM_SIZE);

    fill(ram, 20, 0xdd);
    if (!recomp_snapshots_load(&s, 0u) || memcmp(ram, second, RAM_SIZE) != 0) {
        return fail(mode, "load did not undo unsaved writes");
    }
#ifdef _WIN32
    if (write_watch) {
        if (!read_file_into(ram + 30 * PAGE)) return fail(mode, "ReadFile into guest RAM");
        if (!recomp_snapshots_load(&s, 0u) || memcmp(ram, second, RAM_SIZE) != 0) {
            return fail(mode, "write watch missed a ReadFile write");
        }
    }
#endif
    fill(ram, 5, 0xee);
    if (!recomp_snapshots_load(&s, 1u) || memcmp(ram, first, RAM_SIZE) != 0) {
        return fail(mode, "load one back");
    }
    if (!recomp_snapshots_load(&s, 1u) || memcmp(ram, initial, RAM_SIZE) != 0) {
        return fail(mode, "load back to init");
    }
    if (recomp_snapshots_load(&s, 1u) || memcmp(ram, initial, RAM_SIZE) != 0) {
        return fail(mode, "load past the history changed RAM");
    }

    /* A third save drops the oldest: two saves back is the limit. */
    ram[0] ^= 1u;
    if (!recomp_snapshots_save(&s)) return fail(mode, "ring save 1");
    memcpy(first, ram, RAM_SIZE);
    ram[PAGE] ^= 1u;
    if (!recomp_snapshots_save(&s)) return fail(mode, "ring save 2");
    ram[2 * PAGE] ^= 1u;
    if (!recomp_snapshots_save(&s)) return fail(mode, "ring save 3");
    if (recomp_snapshots_load(&s, 3u)) return fail(mode, "load past a full ring");
    if (!recomp_snapshots_load(&s, 2u) || memcmp(ram, first, RAM_SIZE) != 0) {
        return fail(mode, "load across the ring");
    }

    /* The hash follows saves, loads and moved ranges as a fresh pass would. */
    const RecompRamRange ignored[] = {{3 * PAGE + 4, 3 * PAGE + 8}, {40 * PAGE, 42 * PAGE}};
    uint64_t hash = 0u, again = 0u;
    if (!recomp_snapshots_hash(&s, ignored, 2u, &hash) || hash != fresh_hash(ram, ignored, 2u)) {
        return fail(mode, "first hash");
    }
    ram[3 * PAGE + 5] ^= 0x10u;
    ram[41 * PAGE + 7] ^= 1u;
    if (!recomp_snapshots_save(&s) || !recomp_snapshots_hash(&s, ignored, 2u, &again) || again != hash) {
        return fail(mode, "an ignored byte changed the hash");
    }
    ram[3 * PAGE + 9] ^= 1u;
    if (!recomp_snapshots_save(&s) || !recomp_snapshots_hash(&s, ignored, 2u, &again) || again == hash ||
        again != fresh_hash(ram, ignored, 2u)) {
        return fail(mode, "hash after a save");
    }
    if (!recomp_snapshots_load(&s, 1u) || !recomp_snapshots_hash(&s, ignored, 2u, &again) || again != hash) {
        return fail(mode, "hash after a load");
    }
    if (!recomp_snapshots_hash(&s, NULL, 0u, &again) || again == hash || again != fresh_hash(ram, NULL, 0u)) {
        return fail(mode, "hash after the ranges moved");
    }
    recomp_snapshots_free(&s);
    return 0;
}

int main(void)
{
    bool write_watch = false;
    uint8_t *ram = recomp_guest_ram_allocate(RAM_SIZE, &write_watch);
    if (ram == NULL) return fail("allocate", "guest RAM");
    int result = run(ram, write_watch, write_watch ? "write watch" : "page compare");
    if (result == 0 && write_watch) {
        /* Both would reset the same tracking, so the second must be refused. */
        RecompGuestSnapshots a, b;
        if (!recomp_snapshots_init(&a, ram, RAM_SIZE, true, 1u)) result = fail("exclusion", "first init");
        else if (recomp_snapshots_init(&b, ram, RAM_SIZE, true, 1u)) result = fail("exclusion", "second write watch");
        recomp_snapshots_free(&a);
        recomp_snapshots_free(&b);
    }
    recomp_guest_ram_free(ram);
    if (result != 0) return result;
    /* The page-compare path runs on every host. */
    uint8_t *plain = calloc(RAM_SIZE, 1u);
    if (plain == NULL) return fail("allocate", "plain RAM");
    result = run(plain, false, "page compare");
    free(plain);
    if (result == 0) printf("guest snapshot: write watch=%d ok\n", write_watch ? 1 : 0);
    return result;
}
