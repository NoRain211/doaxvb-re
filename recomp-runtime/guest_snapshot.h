#ifndef RECOMP_GUEST_SNAPSHOT_H
#define RECOMP_GUEST_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Save points of guest RAM for rollback. Each save stores only the pages
   written since the previous one; a fight frame writes about 320 of the
   16,384. Callers must stop every host thread that writes guest RAM (guest
   threads, host read workers) around init, save and load. */
enum { RECOMP_SNAPSHOT_PAGE_SIZE = 0x1000u };
enum { RECOMP_SNAPSHOT_HASH_RANGES = 64u };

/* Guest RAM offsets [start, stop). */
typedef struct RecompRamRange {
    size_t start, stop;
} RecompRamRange;

typedef struct RecompSnapshotRecord {
    uint32_t *pages;
    uint8_t *data;
    size_t count;
    size_t capacity;
} RecompSnapshotRecord;

typedef struct RecompGuestSnapshots {
    uint8_t *ram;
    size_t size;
    bool write_watch;
    uint8_t *shadow;      /* RAM as of the newest save */
    uint32_t *dirty_pages;
    void **watch_addresses;
    /* Each record holds the pages that step one save back to the one before. */
    RecompSnapshotRecord *records;
    size_t slot_count;
    size_t newest;
    size_t depth;
    /* recomp_snapshots_hash: each page's hash, their sum and the ranges read
       as zero. */
    uint64_t *page_hashes;
    uint64_t hash;
    RecompRamRange hash_ignored[RECOMP_SNAPSHOT_HASH_RANGES];
    size_t hash_ignored_count;
} RecompGuestSnapshots;

/* Zeroed guest RAM. On Windows the block tracks writes, which must be chosen
   at allocation; *write_watch reports whether it does. */
uint8_t *recomp_guest_ram_allocate(size_t size, bool *write_watch);
void recomp_guest_ram_free(uint8_t *ram);

/* Takes the first save point. load can then go up to slot_count saves back.
   Fails when write_watch is set and the RAM does not track writes, or another
   live instance already uses the tracking. */
bool recomp_snapshots_init(RecompGuestSnapshots *snapshots, uint8_t *ram, size_t size,
    bool write_watch, size_t slot_count);
/* On allocation failure the history is dropped and the current RAM becomes
   the only save point. */
bool recomp_snapshots_save(RecompGuestSnapshots *snapshots);
/* Restores the save `back` saves before the newest (0 is the newest) and
   discards the later ones. Out of range fails without touching RAM. */
bool recomp_snapshots_load(RecompGuestSnapshots *snapshots, size_t back);
/* A hash of the RAM as of the newest save or load, reading the ignored
   ranges (at most RECOMP_SNAPSHOT_HASH_RANGES) as zero. The first call
   hashes every page; from then on each save and load hashes the pages it
   changes while they are in cache, and a call hashes only the pages of its
   ranges and the last call's when those differ. False when out of memory. */
bool recomp_snapshots_hash(RecompGuestSnapshots *snapshots, const RecompRamRange *ignored,
    size_t count, uint64_t *hash);
void recomp_snapshots_free(RecompGuestSnapshots *snapshots);

#ifdef __cplusplus
}
#endif

#endif
