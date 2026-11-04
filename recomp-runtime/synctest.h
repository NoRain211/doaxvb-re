#ifndef DOAXBV_RECOMP_SYNCTEST_H
#define DOAXBV_RECOMP_SYNCTEST_H

#include "guest_snapshot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One piece of host or guest state copied at a frame boundary. save writes
   size bytes; restore reads them back. */
typedef struct RecompSyncTestModule {
    const char *name;
    size_t size;
    void (*save)(void *state);
    void (*restore)(const void *state);
    /* Optional comparison; NULL means memcmp. */
    bool (*equal)(const void *left, const void *right, size_t size);
    /* Optional: false when this saved state cannot be rewound faithfully. */
    bool (*restorable)(const void *saved, size_t size);
    /* Host-scheduled state that is not part of the simulation. */
    bool ignore;
} RecompSyncTestModule;

typedef enum RecompSyncTestResult {
    RECOMP_SYNCTEST_RUNNING,
    RECOMP_SYNCTEST_MATCH,
    RECOMP_SYNCTEST_MISMATCH,
    RECOMP_SYNCTEST_SKIP,
} RecompSyncTestResult;

void recomp_synctest_set_modules(const RecompSyncTestModule *modules, size_t count);
/* The runtime's real state table; defined beside the runtime, not the core. */
void recomp_synctest_install_default_modules(void);
/* Extra state the runner owns (its scripted input schedule). */
void recomp_synctest_set_extra_state(
    size_t size, void (*save)(void *), void (*restore)(const void *));
/* Guest RAM rewinds through page snapshots (guest_snapshot.h) rather than a
   module copy. equal, when set, skips host-scheduled ranges in the compare. */
void recomp_synctest_set_guest_ram(uint8_t *ram, size_t size, bool write_watch);
void recomp_synctest_set_ram_equal(bool (*equal)(const void *left, const void *right, size_t size));

/* RECOMP_SYNCTEST=1 checks one frame, =loop keeps checking, and
   RECOMP_SYNCTEST_FRAMES sets the replay length. */
bool recomp_synctest_enabled(void);
bool recomp_synctest_loop(void);
void recomp_synctest_configure(bool enabled, unsigned frames, bool loop);
/* Call at the top of each scheduler loop, after the frame-locked clock advances. */
RecompSyncTestResult recomp_synctest_frame(uint32_t frame);

/* Rollback save points of the same modules and guest RAM, for chosen frames.
   SyncTest and the ring share the RAM write tracking, so only one of them
   may run. */
bool recomp_rollback_ring_init(size_t frames);
/* Saves the state as `frame` starts. Frames between saves need none; their
   changes fold into the next save. False drops the history; treat it as fatal. */
bool recomp_rollback_ring_save(uint32_t frame);
/* Restores the save made for `frame` and discards later ones. False, with
   nothing changed, when no kept save has that frame or it cannot be rewound. */
bool recomp_rollback_ring_load(uint32_t frame);
/* A hash of guest RAM as of the ring's newest save or load, reading the
   ignored ranges as zero (recomp_snapshots_hash). */
bool recomp_rollback_ring_hash(const RecompRamRange *ignored, size_t count, uint64_t *hash);
/* Zeroes the guest RAM ranges SyncTest ignores (host-scheduled state). */
void recomp_synctest_mask_ignored(uint8_t *ram, size_t size);
/* Those ranges, sorted, into RECOMP_FIBER_MAX_COUNT entries (fiber_model.h). */
size_t recomp_synctest_ignored_ranges(RecompRamRange *ranges);

#ifdef __cplusplus
}
#endif

#endif
