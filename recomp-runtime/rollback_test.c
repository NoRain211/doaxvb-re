#include "rollback.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char *name, bool passed)
{
    if (!passed) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

static RecompInputGamepad pad(uint16_t buttons)
{
    RecompInputGamepad value;
    memset(&value, 0, sizeof value);
    value.buttons = buttons;
    return value;
}

int main(void)
{
    static RecompRollbackSession s;
    RecompInputGamepad local, remote;

    recomp_rollback_init(&s, 0u);
    check("needs local pad", !recomp_rollback_can_run(&s));
    for (uint16_t i = 0u; i < 4u; ++i) {
        const RecompInputGamepad value = pad(i);
        recomp_rollback_add_local(&s, &value);
    }
    const RecompInputGamepad one = pad(1u), two = pad(2u), three = pad(3u);
    recomp_rollback_add_remote(&s, &one);
    recomp_rollback_run(&s, &local, &remote);
    check("confirmed pad", remote.buttons == 1u && local.buttons == 0u);
    recomp_rollback_run(&s, &local, &remote);
    check("prediction repeats", remote.buttons == 1u && local.buttons == 1u);
    recomp_rollback_run(&s, &local, &remote);
    check("no rewind before input", recomp_rollback_take_rewind(&s) == RECOMP_ROLLBACK_NONE);

    // Frame 1 was predicted right and frame 2 wrong, so the rewind is to 2.
    recomp_rollback_add_remote(&s, &one);
    recomp_rollback_add_remote(&s, &two);
    recomp_rollback_add_remote(&s, &three);
    check("rewind to first miss", recomp_rollback_take_rewind(&s) == 2u && s.frame == 2u);
    check("rewind taken once", recomp_rollback_take_rewind(&s) == RECOMP_ROLLBACK_NONE);
    recomp_rollback_run(&s, &local, &remote);
    check("replay uses arrived pad", remote.buttons == 2u && local.buttons == 2u);
    recomp_rollback_run(&s, &local, &remote);
    check("replay reaches arrived frame", remote.buttons == 3u && local.buttons == 3u);

    // The local side stops WINDOW frames past the last remote pad.
    recomp_rollback_init(&s, 0u);
    for (unsigned i = 0u; i < 20u; ++i) recomp_rollback_add_local(&s, &local);
    unsigned ran = 0u;
    while (recomp_rollback_can_run(&s)) {
        recomp_rollback_run(&s, &local, &remote);
        ++ran;
    }
    check("window stalls", ran == RECOMP_ROLLBACK_WINDOW);
    check("late pad fits window", recomp_rollback_add_remote(&s, &two));
    check("rewind to frame 0", recomp_rollback_take_rewind(&s) == 0u);

    // History keeps the window behind the current frame, so the rest is ahead.
    recomp_rollback_init(&s, 0u);
    unsigned added = 0u;
    while (recomp_rollback_add_local(&s, &local)) ++added;
    check("history bound", added == RECOMP_ROLLBACK_HISTORY - RECOMP_ROLLBACK_WINDOW);

    // Input delay: both sides start with neutral pads already known.
    recomp_rollback_init(&s, 2u);
    check("delay runs at once", recomp_rollback_can_run(&s));
    recomp_rollback_run(&s, &local, &remote);
    check("delay frames neutral", local.buttons == 0u && remote.buttons == 0u);

    // Lockstep: window 0 runs only frames whose remote pad has arrived.
    recomp_rollback_init(&s, 0u);
    s.window = 0u;
    recomp_rollback_add_local(&s, &local);
    check("lockstep waits for remote", !recomp_rollback_can_run(&s));
    recomp_rollback_add_remote(&s, &two);
    check("lockstep runs confirmed frame", recomp_rollback_can_run(&s));

    if (failures != 0) return 1;
    puts("PASS rollback predicts remote pads and rewinds to the first miss");
    return 0;
}
