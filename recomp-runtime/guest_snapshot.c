#include "guest_snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

enum { PAGE = RECOMP_SNAPSHOT_PAGE_SIZE };

static void rehash_page(RecompGuestSnapshots *s, size_t page);
static void rehash_all(RecompGuestSnapshots *s);

/* Each instance resets the write tracking, so a second one would lose the
   first one's pages. */
static const RecompGuestSnapshots *write_watcher;

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

static bool reset_tracking(RecompGuestSnapshots *s)
{
#ifdef _WIN32
    if (s->write_watch) return ResetWriteWatch(s->ram, s->size) == 0u;
    return true;
#else
    return !s->write_watch; /* no host tracking: a write-watch request must fail */
#endif
}

/* Fills dirty_pages with the pages that differ from the shadow, which are
   those changed since the newest save. Returns SIZE_MAX on failure. */
static size_t collect_dirty(RecompGuestSnapshots *s)
{
    /* shortcut: without write watch every page is compared with the shadow,
       a few ms per 64 MB save; use userfaultfd write-protect on Linux when
       rollback runs there. */
    size_t written = s->size / PAGE, count = 0u;
#ifdef _WIN32
    if (s->write_watch) {
        ULONG_PTR found = written;
        DWORD granularity = 0u;
        if (GetWriteWatch(0u, s->ram, s->size, s->watch_addresses, &found, &granularity) != 0u ||
            granularity != PAGE) {
            return SIZE_MAX;
        }
        written = found;
    }
#endif
    for (size_t i = 0u; i < written; ++i) {
        const size_t page = s->write_watch ? (size_t)((uint8_t *)s->watch_addresses[i] - s->ram) / PAGE : i;
        if (memcmp(s->ram + page * PAGE, s->shadow + page * PAGE, PAGE) != 0) {
            s->dirty_pages[count++] = (uint32_t)page;
        }
    }
    /* Tracking starts again only once its list is mostly pages that no longer
       change. After a reset the first write to each page faults, about 0.7 us:
       0.22 ms for a fight frame's 320 pages, against 0.1 us to compare one. */
    if (s->write_watch && written > 2u * count + 64u) reset_tracking(s);
    return count;
}

/* A page whose shadow changed is hashed again at the next recomp_snapshots_hash. */
static void hash_stale(RecompGuestSnapshots *s, size_t page)
{
    if (s->hash_stale != NULL) s->hash_stale[page / 8u] |= (uint8_t)(1u << (page % 8u));
}

bool recomp_snapshots_init(RecompGuestSnapshots *s, uint8_t *ram, size_t size,
    bool write_watch, size_t slot_count)
{
    if (write_watcher == s) write_watcher = NULL;
    memset(s, 0, sizeof *s);
    if (ram == NULL || size == 0u || size % PAGE != 0u || size / PAGE > UINT32_MAX ||
        slot_count == 0u) {
        return false;
    }
    if (write_watch && write_watcher != NULL) {
        fprintf(stderr, "recomp snapshots: guest RAM write watch is already in use\n");
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
    s->restored = malloc((page_count + 7u) / 8u);
    if (write_watch) s->watch_addresses = malloc(page_count * sizeof *s->watch_addresses);
    if (s->shadow == NULL || s->dirty_pages == NULL || s->records == NULL || s->restored == NULL ||
        (write_watch && s->watch_addresses == NULL)) {
        recomp_snapshots_free(s);
        return false;
    }
    /* Reset before copying: a write in between is only reported, never lost. */
    if (!reset_tracking(s)) {
        recomp_snapshots_free(s);
        return false;
    }
    memcpy(s->shadow, ram, size);
    if (write_watch) write_watcher = s;
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
        if (s->hash_stale != NULL) memset(s->hash_stale, 0xff, (s->size / PAGE + 7u) / 8u);
        return false;
    }
    for (size_t i = 0u; i < count; ++i) {
        const size_t offset = (size_t)s->dirty_pages[i] * PAGE;
        record->pages[i] = s->dirty_pages[i];
        memcpy(record->data + i * PAGE, s->shadow + offset, PAGE);
        memcpy(s->shadow + offset, s->ram + offset, PAGE);
        hash_stale(s, s->dirty_pages[i]);
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
    /* A page takes its value from the oldest undone record that holds it, else
       from the shadow. Walking oldest first writes each page once; a fight
       writes most of the same pages every frame. */
    memset(s->restored, 0, (s->size / PAGE + 7u) / 8u);
    for (size_t step = back; step-- > 0u;) {
        RecompSnapshotRecord *record = &s->records[(s->newest + s->slot_count - step) % s->slot_count];
        for (size_t i = 0u; i < record->count; ++i) {
            const uint32_t page = record->pages[i];
            if ((s->restored[page / 8u] & (1u << (page % 8u))) != 0u) continue;
            s->restored[page / 8u] |= (uint8_t)(1u << (page % 8u));
            const size_t offset = (size_t)page * PAGE;
            memcpy(s->ram + offset, record->data + i * PAGE, PAGE);
            memcpy(s->shadow + offset, record->data + i * PAGE, PAGE);
            hash_stale(s, page);
        }
        record->count = 0u;
    }
    for (size_t i = 0u; i < count; ++i) {
        const uint32_t page = s->dirty_pages[i];
        if ((s->restored[page / 8u] & (1u << (page % 8u))) != 0u) continue;
        memcpy(s->ram + (size_t)page * PAGE, s->shadow + (size_t)page * PAGE, PAGE);
    }
    s->newest = (s->newest + s->slot_count - back) % s->slot_count;
    s->depth -= back;
    /* The restore's own writes leave RAM equal to the shadow, so the next save drops them. */
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
    /* Ranges that together cover the page read as zero like one range that does. */
    static const uint8_t zero_page[PAGE];
    if (data == masked && memcmp(masked, zero_page, PAGE) == 0) data = NULL;
    const uint64_t h = data != NULL ? page_hash(data, page) : 0u;
    s->hash += h - s->page_hashes[page];
    s->page_hashes[page] = h;
}

static void rehash_all(RecompGuestSnapshots *s)
{
    for (size_t page = 0u; page < s->size / PAGE; ++page) rehash_page(s, page);
    memset(s->hash_stale, 0, (s->size / PAGE + 7u) / 8u);
}

static void rehash_stale(RecompGuestSnapshots *s)
{
    for (size_t byte = 0u; byte < (s->size / PAGE + 7u) / 8u; ++byte) {
        /* The last byte's high bits are past the last page after a 0xff fill. */
        for (unsigned bit = 0u; s->hash_stale[byte] != 0u && bit < 8u && byte * 8u + bit < s->size / PAGE; ++bit) {
            if ((s->hash_stale[byte] & (1u << bit)) != 0u) rehash_page(s, byte * 8u + bit);
        }
        s->hash_stale[byte] = 0u;
    }
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
        s->hash_stale = malloc((s->size / PAGE + 7u) / 8u);
        if (s->page_hashes == NULL || s->hash_stale == NULL) {
            free(s->page_hashes);
            free(s->hash_stale);
            s->page_hashes = NULL;
            s->hash_stale = NULL;
            return false;
        }
        s->hash = 0u;
        rehash_all(s);
    } else {
        rehash_stale(s);
        if (moved) {
            /* A range that moved changes the mask of pages no save touched. */
            rehash_ranges(s, old, old_count);
            rehash_ranges(s, ignored, count);
        }
    }
    *hash = s->hash;
    return true;
}

void recomp_snapshots_free(RecompGuestSnapshots *s)
{
    if (write_watcher == s) write_watcher = NULL;
    if (s->records != NULL) {
        for (size_t i = 0u; i < s->slot_count; ++i) {
            free(s->records[i].pages);
            free(s->records[i].data);
        }
    }
    free(s->records);
    free(s->shadow);
    free(s->dirty_pages);
    free(s->restored);
    free(s->watch_addresses);
    free(s->page_hashes);
    free(s->hash_stale);
    memset(s, 0, sizeof *s);
}
