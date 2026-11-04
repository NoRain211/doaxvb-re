#include "guest_snapshot.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

enum { PAGE = RECOMP_SNAPSHOT_PAGE_SIZE };

static void rehash_page(RecompGuestSnapshots *s, size_t page);
static void rehash_all(RecompGuestSnapshots *s);

uint8_t *recomp_guest_ram_allocate(size_t size, bool *write_watch)
{
#ifdef _WIN32
    *write_watch = true;
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
#else
    *write_watch = false;
    return calloc(size, 1u);
#endif
}

void recomp_guest_ram_free(uint8_t *ram)
{
#ifdef _WIN32
    if (ram != NULL) VirtualFree(ram, 0u, MEM_RELEASE);
#else
    free(ram);
#endif
}

static void reset_tracking(RecompGuestSnapshots *s)
{
#ifdef _WIN32
    if (s->write_watch) ResetWriteWatch(s->ram, s->size);
#else
    (void)s;
#endif
}

/* Fills dirty_pages with the pages written since the newest save and resets
   tracking. Returns SIZE_MAX on failure. */
static size_t collect_dirty(RecompGuestSnapshots *s)
{
    const size_t page_count = s->size / PAGE;
#ifdef _WIN32
    if (s->write_watch) {
        ULONG_PTR count = page_count;
        DWORD granularity = 0u;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, s->ram, s->size, s->watch_addresses,
                &count, &granularity) != 0u || granularity != PAGE) {
            return SIZE_MAX;
        }
        for (size_t i = 0u; i < count; ++i) {
            s->dirty_pages[i] = (uint32_t)(((uint8_t *)s->watch_addresses[i] - s->ram) / PAGE);
        }
        return count;
    }
#endif
    /* shortcut: without write watch every page is compared with the shadow,
       a few ms per 64 MB save; use userfaultfd write-protect on Linux when
       rollback runs there. */
    size_t count = 0u;
    for (size_t page = 0u; page < page_count; ++page) {
        if (memcmp(s->ram + page * PAGE, s->shadow + page * PAGE, PAGE) != 0) {
            s->dirty_pages[count++] = (uint32_t)page;
        }
    }
    return count;
}

bool recomp_snapshots_init(RecompGuestSnapshots *s, uint8_t *ram, size_t size,
    bool write_watch, size_t slot_count)
{
    memset(s, 0, sizeof *s);
    if (ram == NULL || size == 0u || size % PAGE != 0u || size / PAGE > UINT32_MAX ||
        slot_count == 0u) {
        return false;
    }
    const size_t page_count = size / PAGE;
    s->ram = ram;
    s->size = size;
    s->write_watch = write_watch;
    s->slot_count = slot_count;
    s->newest = slot_count - 1u;
    s->shadow = malloc(size);
    s->dirty_pages = malloc(page_count * sizeof *s->dirty_pages);
    s->records = calloc(slot_count, sizeof *s->records);
    if (write_watch) s->watch_addresses = malloc(page_count * sizeof *s->watch_addresses);
    if (s->shadow == NULL || s->dirty_pages == NULL || s->records == NULL ||
        (write_watch && s->watch_addresses == NULL)) {
        recomp_snapshots_free(s);
        return false;
    }
    /* Reset before copying: a write in between is only reported, never lost. */
    reset_tracking(s);
    memcpy(s->shadow, ram, size);
    return true;
}

static bool reserve(RecompSnapshotRecord *record, size_t count)
{
    if (count <= record->capacity) return true;
    uint32_t *pages = realloc(record->pages, count * sizeof *pages);
    if (pages == NULL) return false;
    record->pages = pages;
    uint8_t *data = realloc(record->data, count * PAGE);
    if (data == NULL) return false;
    record->data = data;
    record->capacity = count;
    return true;
}

bool recomp_snapshots_save(RecompGuestSnapshots *s)
{
    const size_t count = collect_dirty(s);
    const size_t next = (s->newest + 1u) % s->slot_count;
    RecompSnapshotRecord *record = &s->records[next];
    if (count == SIZE_MAX || !reserve(record, count)) {
        s->depth = 0u;
        reset_tracking(s);
        memcpy(s->shadow, s->ram, s->size);
        if (s->page_hashes != NULL) rehash_all(s);
        return false;
    }
    for (size_t i = 0u; i < count; ++i) {
        const size_t offset = (size_t)s->dirty_pages[i] * PAGE;
        record->pages[i] = s->dirty_pages[i];
        memcpy(record->data + i * PAGE, s->shadow + offset, PAGE);
        memcpy(s->shadow + offset, s->ram + offset, PAGE);
        if (s->page_hashes != NULL) rehash_page(s, s->dirty_pages[i]);
    }
    record->count = count;
    s->newest = next;
    if (s->depth < s->slot_count) ++s->depth;
    return true;
}

bool recomp_snapshots_load(RecompGuestSnapshots *s, size_t back)
{
    if (back > s->depth) return false;
    const size_t count = collect_dirty(s);
    if (count == SIZE_MAX) return false;
    for (size_t i = 0u; i < count; ++i) {
        const size_t offset = (size_t)s->dirty_pages[i] * PAGE;
        memcpy(s->ram + offset, s->shadow + offset, PAGE);
    }
    for (size_t step = 0u; step < back; ++step) {
        RecompSnapshotRecord *record = &s->records[s->newest];
        for (size_t i = 0u; i < record->count; ++i) {
            const size_t offset = (size_t)record->pages[i] * PAGE;
            memcpy(s->ram + offset, record->data + i * PAGE, PAGE);
            memcpy(s->shadow + offset, record->data + i * PAGE, PAGE);
            if (s->page_hashes != NULL) rehash_page(s, record->pages[i]);
        }
        record->count = 0u;
        s->newest = (s->newest + s->slot_count - 1u) % s->slot_count;
        --s->depth;
    }
    /* The restore's own writes are not guest changes. */
    reset_tracking(s);
    return true;
}

static uint64_t rotl(uint64_t x, unsigned r) { return x << r | x >> (64u - r); }

/* Eight multiply-rotate lanes, seeded with the page's index so equal pages
   at different addresses differ; about 20 GB/s on a cached page. */
static uint64_t page_hash(const uint8_t *data, uint64_t seed)
{
    const uint64_t k1 = 0x9e3779b185ebca87u, k2 = 0xc2b2ae3d27d4eb4fu;
    uint64_t lane[8];
    for (unsigned j = 0u; j < 8u; ++j) lane[j] = seed + j * k2;
    for (size_t i = 0u; i < PAGE; i += 64u) {
        for (unsigned j = 0u; j < 8u; ++j) {
            uint64_t word;
            memcpy(&word, data + i + 8u * j, sizeof word);
            lane[j] = rotl((lane[j] ^ word) * k1, 29u);
        }
    }
    uint64_t h = 0u;
    for (unsigned j = 0u; j < 8u; ++j) h = (h ^ lane[j]) * k2;
    h ^= h >> 33;
    h *= k1;
    return h ^ h >> 29;
}

/* Hashes one page of the shadow, with the stored ranges read as zero, into the sum. */
static void rehash_page(RecompGuestSnapshots *s, size_t page)
{
    const size_t offset = page * PAGE;
    const uint8_t *data = s->shadow + offset;
    uint8_t masked[PAGE];
    for (size_t i = 0u; i < s->hash_ignored_count; ++i) {
        const RecompRamRange *range = &s->hash_ignored[i];
        const size_t start = range->start > offset ? range->start : offset;
        const size_t stop = range->stop < offset + PAGE ? range->stop : offset + PAGE;
        if (start >= stop) continue;
        if (stop - start == PAGE) {
            data = NULL; /* wholly ignored: a constant 0, so moving stacks cost little */
            break;
        }
        if (data != masked) {
            memcpy(masked, data, PAGE);
            data = masked;
        }
        memset(masked + (start - offset), 0, stop - start);
    }
    const uint64_t h = data != NULL ? page_hash(data, page) : 0u;
    s->hash += h - s->page_hashes[page];
    s->page_hashes[page] = h;
}

static void rehash_all(RecompGuestSnapshots *s)
{
    for (size_t page = 0u; page < s->size / PAGE; ++page) rehash_page(s, page);
}

static void rehash_ranges(RecompGuestSnapshots *s, const RecompRamRange *ranges, size_t count)
{
    for (size_t i = 0u; i < count; ++i)
        for (size_t page = ranges[i].start / PAGE; page < s->size / PAGE && page * PAGE < ranges[i].stop; ++page)
            rehash_page(s, page);
}

bool recomp_snapshots_hash(RecompGuestSnapshots *s, const RecompRamRange *ignored, size_t count, uint64_t *hash)
{
    if (count > RECOMP_SNAPSHOT_HASH_RANGES) return false;
    const bool moved = count != s->hash_ignored_count ||
        (count != 0u && memcmp(ignored, s->hash_ignored, count * sizeof *ignored) != 0);
    RecompRamRange old[RECOMP_SNAPSHOT_HASH_RANGES];
    const size_t old_count = s->hash_ignored_count;
    if (old_count != 0u) memcpy(old, s->hash_ignored, old_count * sizeof *old);
    if (count != 0u) memcpy(s->hash_ignored, ignored, count * sizeof *ignored);
    s->hash_ignored_count = count;
    if (s->page_hashes == NULL) {
        s->page_hashes = calloc(s->size / PAGE, sizeof *s->page_hashes);
        if (s->page_hashes == NULL) return false;
        s->hash = 0u;
        rehash_all(s);
    } else if (moved) {
        /* A range that moved changes the mask of pages no save touched. */
        rehash_ranges(s, old, old_count);
        rehash_ranges(s, ignored, count);
    }
    *hash = s->hash;
    return true;
}

void recomp_snapshots_free(RecompGuestSnapshots *s)
{
    if (s->records != NULL) {
        for (size_t i = 0u; i < s->slot_count; ++i) {
            free(s->records[i].pages);
            free(s->records[i].data);
        }
    }
    free(s->records);
    free(s->shadow);
    free(s->dirty_pages);
    free(s->watch_addresses);
    free(s->page_hashes);
    memset(s, 0, sizeof *s);
}
