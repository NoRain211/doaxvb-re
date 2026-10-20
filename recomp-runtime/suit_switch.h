#ifndef DOAXBV_RECOMP_SUIT_SWITCH_H
#define DOAXBV_RECOMP_SUIT_SWITCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Changing a character's swimsuit while the scene is on screen. The game
   loads a suit over the one a character wears, in the same memory, so
   nothing may draw her from the moment the loader starts on the new suit
   until the game has bound it to her. A switch therefore runs in phases:

   waiting   for the next camera build, in the middle of a frame's drawing;
             there the old suit is let go and the new one asked for, and
             from the end of that turn only the loader task runs
   loading   until the loader reports the new suit bound
   settling  one more frame with only the loader, then the scene runs again

   Letting the suit go only marks its memory free: the rest of the turn
   still draws the old suit, intact, and the loader cannot start on the new
   one before its own turn. A switch that does not get that far within
   RECOMP_SUIT_SWITCH_LIMIT frames gives up. */
enum {
    RECOMP_SUIT_SWITCH_LIMIT = 600,
};

typedef enum RecompSuitPhase {
    RECOMP_SUIT_IDLE = 0,
    RECOMP_SUIT_WAITING,
    RECOMP_SUIT_LOADING,
    RECOMP_SUIT_SETTLING,
} RecompSuitPhase;

typedef enum RecompSuitAction {
    RECOMP_SUIT_ACTION_NONE = 0,
    /* Let the old suit go, ask for the new one and run only the loader. */
    RECOMP_SUIT_ACTION_LOAD,
    /* The new suit is on: let the scene run again. */
    RECOMP_SUIT_ACTION_FINISH,
    /* The switch never got there: let the scene run again anyway. */
    RECOMP_SUIT_ACTION_GIVE_UP,
} RecompSuitAction;

typedef struct RecompSuitSwitch {
    RecompSuitPhase phase;
    uint32_t slot;
    uint16_t suit;
    /* The frame the current phase began. */
    uint32_t since;
} RecompSuitSwitch;

/* Starts a switch of `slot` to `suit`. False while another is under way. */
bool recomp_suit_switch_begin(RecompSuitSwitch *state, uint32_t slot,
    uint16_t suit, uint32_t frame);
/* The game builds a camera in frame `frame`: what to do there. A waiting
   switch is dropped unless `ready`: the slot still holds a girl and the
   game's request record for it is idle. */
RecompSuitAction recomp_suit_switch_camera_built(RecompSuitSwitch *state,
    uint32_t frame, bool ready);
/* Between task turns in frame `frame`; `loaded` when the loader reports
   the slot's suit read and bound. */
RecompSuitAction recomp_suit_switch_tick(RecompSuitSwitch *state,
    uint32_t frame, bool loaded);

/* Swimsuit numbers run from 1 to below `end`. The next (step > 0) or
   previous suit after `current` that `wearable` accepts, wrapping, or
   `current` when there is none. */
typedef bool (*RecompSuitWearable)(uint16_t suit, void *context);
uint16_t recomp_suit_switch_next(uint16_t current, int step, uint16_t end,
    RecompSuitWearable wearable, void *context);

#ifdef __cplusplus
}
#endif

#endif
