#include "suit_switch.h"

#include <stdio.h>

static int failures;

static void check(bool condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "suit switch test failed: %s\n", what);
        ++failures;
    }
}

/* Suits 3, 5 and 9 of 1..9 fit. */
static bool fits(uint16_t suit, void *context)
{
    (void)context;
    return suit == 3u || suit == 5u || suit == 9u;
}

static bool none(uint16_t suit, void *context)
{
    (void)suit;
    (void)context;
    return false;
}

int main(void)
{
    check(recomp_suit_switch_next(3u, 1, 10u, fits, NULL) == 5u, "next");
    check(recomp_suit_switch_next(9u, 1, 10u, fits, NULL) == 3u, "next wraps");
    check(recomp_suit_switch_next(3u, -1, 10u, fits, NULL) == 9u, "previous wraps");
    check(recomp_suit_switch_next(4u, -1, 10u, fits, NULL) == 3u, "previous from one that does not fit");
    check(recomp_suit_switch_next(0u, 1, 10u, fits, NULL) == 3u, "next from none");
    check(recomp_suit_switch_next(3u, 0, 10u, fits, NULL) == 3u, "no step");
    check(recomp_suit_switch_next(4u, 1, 10u, none, NULL) == 4u, "nothing fits");

    RecompSuitSwitch state = {0};
    check(recomp_suit_switch_tick(&state, 10u, true) == RECOMP_SUIT_ACTION_NONE, "idle does nothing");
    check(recomp_suit_switch_camera_built(&state, 10u, true) == RECOMP_SUIT_ACTION_NONE, "idle loads nothing");
    check(recomp_suit_switch_begin(&state, 1u, 42u, 50u), "begins before the slot changes");
    check(recomp_suit_switch_camera_built(&state, 51u, false) == RECOMP_SUIT_ACTION_NONE,
        "loads nothing once the slot is not ready");
    check(state.phase == RECOMP_SUIT_IDLE, "dropped");
    check(recomp_suit_switch_begin(&state, 1u, 42u, 100u), "begins");
    check(!recomp_suit_switch_begin(&state, 0u, 7u, 100u), "one switch at a time");
    check(recomp_suit_switch_tick(&state, 100u, true) == RECOMP_SUIT_ACTION_NONE, "waits for a camera build");
    check(state.phase == RECOMP_SUIT_WAITING, "still waiting: a load not asked for does not count");
    check(recomp_suit_switch_camera_built(&state, 101u, true) == RECOMP_SUIT_ACTION_LOAD, "loads at the camera build");
    check(recomp_suit_switch_camera_built(&state, 101u, true) == RECOMP_SUIT_ACTION_NONE, "once");
    check(recomp_suit_switch_tick(&state, 102u, false) == RECOMP_SUIT_ACTION_NONE, "loading");
    check(recomp_suit_switch_tick(&state, 104u, true) == RECOMP_SUIT_ACTION_NONE, "bound");
    check(state.phase == RECOMP_SUIT_SETTLING, "settling");
    check(recomp_suit_switch_tick(&state, 104u, true) == RECOMP_SUIT_ACTION_NONE, "settles a frame");
    check(recomp_suit_switch_tick(&state, 105u, true) == RECOMP_SUIT_ACTION_FINISH, "then finishes");
    check(state.phase == RECOMP_SUIT_IDLE, "idle again");

    check(recomp_suit_switch_begin(&state, 0u, 7u, 200u), "begins again");
    check(recomp_suit_switch_tick(&state, 200u + RECOMP_SUIT_SWITCH_LIMIT, false) ==
        RECOMP_SUIT_ACTION_GIVE_UP, "gives up without a camera build");
    check(recomp_suit_switch_begin(&state, 0u, 7u, 300u), "and again");
    check(recomp_suit_switch_camera_built(&state, 301u, true) == RECOMP_SUIT_ACTION_LOAD, "loads");
    check(recomp_suit_switch_tick(&state, 301u + RECOMP_SUIT_SWITCH_LIMIT, false) ==
        RECOMP_SUIT_ACTION_GIVE_UP, "gives up on a load that never binds");
    check(state.phase == RECOMP_SUIT_IDLE, "idle after giving up");

    if (failures == 0) printf("suit switch test passed\n");
    return failures == 0 ? 0 : 1;
}
