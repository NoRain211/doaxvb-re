#include "suit_switch.h"

#include <stddef.h>

bool recomp_suit_switch_begin(RecompSuitSwitch *state, uint32_t slot,
    uint16_t suit, uint32_t frame)
{
    if (state->phase != RECOMP_SUIT_IDLE) return false;
    state->phase = RECOMP_SUIT_WAITING;
    state->slot = slot;
    state->suit = suit;
    state->since = frame;
    return true;
}

RecompSuitAction recomp_suit_switch_camera_built(RecompSuitSwitch *state,
    uint32_t frame, bool ready)
{
    if (state->phase != RECOMP_SUIT_WAITING) return RECOMP_SUIT_ACTION_NONE;
    if (!ready) {
        state->phase = RECOMP_SUIT_IDLE;
        return RECOMP_SUIT_ACTION_NONE;
    }
    state->phase = RECOMP_SUIT_LOADING;
    state->since = frame;
    return RECOMP_SUIT_ACTION_LOAD;
}

RecompSuitAction recomp_suit_switch_tick(RecompSuitSwitch *state,
    uint32_t frame, bool loaded)
{
    switch (state->phase) {
    case RECOMP_SUIT_IDLE:
        break;
    case RECOMP_SUIT_WAITING:
    case RECOMP_SUIT_LOADING:
        if (state->phase == RECOMP_SUIT_LOADING && loaded) {
            state->phase = RECOMP_SUIT_SETTLING;
            state->since = frame;
            break;
        }
        if (frame - state->since < RECOMP_SUIT_SWITCH_LIMIT) break;
        state->phase = RECOMP_SUIT_IDLE;
        return RECOMP_SUIT_ACTION_GIVE_UP;
    case RECOMP_SUIT_SETTLING:
        if (frame == state->since) break;
        state->phase = RECOMP_SUIT_IDLE;
        return RECOMP_SUIT_ACTION_FINISH;
    }
    return RECOMP_SUIT_ACTION_NONE;
}

uint16_t recomp_suit_switch_next(uint16_t current, int step, uint16_t end,
    RecompSuitWearable wearable, void *context)
{
    if (step == 0 || end < 2u || wearable == NULL) return current;
    uint16_t suit = current;
    for (uint32_t tries = 1u; tries < end; ++tries) {
        if (step > 0) suit = suit + 1u >= end ? 1u : (uint16_t)(suit + 1u);
        else suit = suit <= 1u || suit >= end ? (uint16_t)(end - 1u) : (uint16_t)(suit - 1u);
        if (suit == current) break;
        if (wearable(suit, context)) return suit;
    }
    return current;
}
