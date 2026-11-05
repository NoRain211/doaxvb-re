#include "synctest.h"

#include <stdio.h>
#include <string.h>

static unsigned char world[64];
static unsigned char extra[4];
static int failures;

static void world_save(void *state) { memcpy(state, world, sizeof world); }
static void world_restore(const void *state) { memcpy(world, state, sizeof world); }
static void extra_save(void *state) { memcpy(state, extra, sizeof extra); }
static void extra_restore(const void *state) { memcpy(extra, state, sizeof extra); }

static const RecompSyncTestModule modules[] = {
    {"world", sizeof world, world_save, world_restore},
};

static int restorable_now = 1;
static unsigned char guarded[8];
static void guarded_save(void *state) { memcpy(state, guarded, sizeof guarded); }
static void guarded_restore(const void *state) { memcpy(guarded, state, sizeof guarded); }
static bool guarded_restorable(const void *state, size_t size)
{
    (void)state; (void)size;
    return restorable_now != 0;
}

static const RecompSyncTestModule skip_modules[] = {
    {"world", sizeof world, world_save, world_restore},
    {"guarded", sizeof guarded, guarded_save, guarded_restore, NULL, guarded_restorable, false},
};

static void check(const char *name, bool passed)
{
    if (!passed) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

static void begin(bool loop)
{
    memset(world, 0, sizeof world);
    memset(extra, 0, sizeof extra);
    recomp_synctest_set_modules(modules, 1u);
    recomp_synctest_configure(true, 1u, loop);
}

int main(void)
{
    recomp_synctest_configure(false, 1u, false);
    check("disabled", !recomp_synctest_enabled());
    check("disabled runs", recomp_synctest_frame(0u) == RECOMP_SYNCTEST_RUNNING);

    begin(false);
    world[0] = 1u;
    check("baseline", recomp_synctest_frame(1u) == RECOMP_SYNCTEST_RUNNING);
    world[0] = 2u;
    check("replay starts", recomp_synctest_frame(2u) == RECOMP_SYNCTEST_RUNNING);
    check("baseline loaded", world[0] == 1u);
    world[0] = 2u;
    check("match", recomp_synctest_frame(3u) == RECOMP_SYNCTEST_MATCH);

    begin(false);
    world[0] = 10u;
    check("baseline 2", recomp_synctest_frame(4u) == RECOMP_SYNCTEST_RUNNING);
    world[0] = 11u;
    check("replay 2", recomp_synctest_frame(5u) == RECOMP_SYNCTEST_RUNNING);
    check("baseline 2 loaded", world[0] == 10u);
    world[0] = 12u;
    check("mismatch", recomp_synctest_frame(6u) == RECOMP_SYNCTEST_MISMATCH);

    // Two-frame replay: the load rewinds both frames, then the same two run.
    memset(world, 0, sizeof world);
    recomp_synctest_set_modules(modules, 1u);
    recomp_synctest_configure(true, 2u, false);
    world[0] = 20u;
    check("two-frame baseline", recomp_synctest_frame(7u) == RECOMP_SYNCTEST_RUNNING);
    world[0] = 21u;
    check("two-frame first", recomp_synctest_frame(8u) == RECOMP_SYNCTEST_RUNNING);
    world[0] = 22u;
    check("two-frame reference", recomp_synctest_frame(9u) == RECOMP_SYNCTEST_RUNNING);
    check("two-frame rewound", world[0] == 20u);
    world[0] = 21u;
    check("two-frame replay first", recomp_synctest_frame(10u) == RECOMP_SYNCTEST_RUNNING);
    world[0] = 22u;
    check("two-frame match", recomp_synctest_frame(11u) == RECOMP_SYNCTEST_MATCH);

    // Loop mode re-arms from the matched state and keeps checking.
    begin(true);
    world[0] = 30u;
    check("loop baseline", recomp_synctest_frame(12u) == RECOMP_SYNCTEST_RUNNING);
    world[0] = 31u;
    check("loop replay", recomp_synctest_frame(13u) == RECOMP_SYNCTEST_RUNNING);
    world[0] = 31u;
    check("loop match", recomp_synctest_frame(14u) == RECOMP_SYNCTEST_MATCH);
    world[0] = 32u;
    check("loop re-armed", recomp_synctest_frame(15u) == RECOMP_SYNCTEST_RUNNING);

    // Runner-owned extra state is snapshotted with everything else.
    begin(false);
    recomp_synctest_set_extra_state(sizeof extra, extra_save, extra_restore);
    extra[0] = 0x5au;
    world[0] = 40u;
    check("extra baseline", recomp_synctest_frame(16u) == RECOMP_SYNCTEST_RUNNING);
    extra[0] = 0x77u;
    world[0] = 41u;
    check("extra replay", recomp_synctest_frame(17u) == RECOMP_SYNCTEST_RUNNING);
    check("extra rewound", extra[0] == 0x5au);
    extra[0] = 0x77u;
    world[0] = 41u;
    check("extra match", recomp_synctest_frame(18u) == RECOMP_SYNCTEST_MATCH);

    // A state that cannot be rewound is skipped, not reported as a mismatch.
    memset(world, 0, sizeof world);
    memset(guarded, 0, sizeof guarded);
    restorable_now = 1;
    recomp_synctest_set_modules(skip_modules, 2u);
    recomp_synctest_configure(true, 1u, false);
    world[0] = 50u;
    check("skip baseline", recomp_synctest_frame(19u) == RECOMP_SYNCTEST_RUNNING);
    restorable_now = 0;
    world[0] = 51u;
    check("skip detected", recomp_synctest_frame(20u) == RECOMP_SYNCTEST_SKIP);
    check("skip keeps live state", world[0] == 51u);

    // Loop mode waits out the transition, then re-arms from the live state.
    restorable_now = 1;
    recomp_synctest_configure(true, 1u, true);
    world[0] = 60u;
    check("skip loop baseline", recomp_synctest_frame(21u) == RECOMP_SYNCTEST_RUNNING);
    restorable_now = 0;
    world[0] = 61u;
    check("skip loop skip", recomp_synctest_frame(22u) == RECOMP_SYNCTEST_RUNNING);
    restorable_now = 1;
    for (uint32_t frame = 23u; frame < 82u; ++frame)
        check("skip loop waits", recomp_synctest_frame(frame) == RECOMP_SYNCTEST_RUNNING);
    check("skip loop re-armed", recomp_synctest_frame(82u) == RECOMP_SYNCTEST_RUNNING);
    check("skip loop replay", recomp_synctest_frame(83u) == RECOMP_SYNCTEST_RUNNING);
    check("skip loop match", recomp_synctest_frame(84u) == RECOMP_SYNCTEST_MATCH);

    // Guest RAM rewinds through page snapshots and is compared after the replay.
    static uint8_t guest[4u * 0x1000u];
    begin(false);
    recomp_synctest_set_guest_ram(guest, sizeof guest, false);
    guest[0] = 1u;
    check("ram baseline", recomp_synctest_frame(87u) == RECOMP_SYNCTEST_RUNNING);
    guest[0x1000u] = 2u;
    check("ram replay starts", recomp_synctest_frame(88u) == RECOMP_SYNCTEST_RUNNING);
    check("ram rewound", guest[0] == 1u && guest[0x1000u] == 0u);
    guest[0x1000u] = 2u;
    check("ram match", recomp_synctest_frame(89u) == RECOMP_SYNCTEST_MATCH);
    begin(false);
    check("ram baseline 2", recomp_synctest_frame(90u) == RECOMP_SYNCTEST_RUNNING);
    guest[0x3000u] = 3u;
    check("ram replay 2", recomp_synctest_frame(91u) == RECOMP_SYNCTEST_RUNNING);
    guest[0x3000u] = 4u;
    check("ram mismatch", recomp_synctest_frame(92u) == RECOMP_SYNCTEST_MISMATCH);

    // The rollback ring keeps the newest saves, made only for some frames, and loads them by frame.
    recomp_synctest_configure(false, 1u, false);
    recomp_synctest_set_modules(modules, 1u);
    recomp_synctest_set_guest_ram(guest, sizeof guest, false);
    check("ring rejects zero frames", !recomp_rollback_ring_init(0u));
    check("ring init", recomp_rollback_ring_init(3u));
    guest[0x3000u] = 0u;
    for (uint8_t frame = 0u; frame < 7u; ++frame) {
        world[0] = frame;
        guest[0x2000u] = frame;
        if (frame % 3u == 2u)
            guest[0x3000u] = frame; // an unsaved frame's write lands in the next save
        else
            check("ring save", recomp_rollback_ring_save(frame));
    }
    world[0] = guest[0x2000u] = 9u;
    check("ring too far back", !recomp_rollback_ring_load(0u));
    check("ring unsaved frame", !recomp_rollback_ring_load(5u) && world[0] == 9u);
    check("ring load", recomp_rollback_ring_load(4u));
    check("ring rewound", world[0] == 4u && guest[0x2000u] == 4u && guest[0x3000u] == 2u);
    check("ring dropped later saves", !recomp_rollback_ring_load(6u));
    check("ring across an unsaved frame", recomp_rollback_ring_load(1u) && world[0] == 1u &&
        guest[0x2000u] == 1u && guest[0x3000u] == 0u);

    if (failures != 0) return 1;
    puts("PASS SyncTest saves, replays and compares frame state");
    return 0;
}
