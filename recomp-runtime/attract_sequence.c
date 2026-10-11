#include "attract_sequence.h"

#ifdef RECOMP_FULL_PROGRAM
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Attract sequence without the movies, opt-in with
   RECOMP_ATTRACT_NO_MOVIES=1.

   Left idle, the game walks a table of seven steps at 0x328C14, four bytes
   each: a kind, a pad byte and a 16-bit parameter. Kind 0 waits, kinds 1 and
   5 play a pre-rendered movie, kind 2 holds the title screen, kind 3 runs
   the real-time volleyball demo and 0xFF jumps to the step in its parameter.
   The sequence is wait, movie, title, movie, title, demo, jump to the first
   title. Booting starts it at the second movie (0x000A4920 asks for the
   first step of kind 5), so the loop is movie, title, demo, title.

   The table is ordinary writable data. With the option on the jump is
   pointed at the second title instead, which leaves the loop as title, demo.
   The movie still plays once after the logos, and every step change left is
   one the game already makes.

   The title step's update at 0x0006B290 holds the screen for 1800 frames
   and starts its fade at frame 1740. With the option on the frame counter
   in the step state (offset 8 of the block the update is handed) is moved
   to the fade once the title has been up for TITLE_HOLD_FRAMES. */
enum {
    STEP_TABLE = 0x00328C14u,
    STEP_SIZE = 4,
    STEP_COUNT = 7,
    STEP_PARAMETER = 2,
    KIND_WAIT = 0,
    KIND_OPENING_MOVIE = 1,
    KIND_TITLE = 2,
    KIND_DEMO = 3,
    KIND_MOVIE = 5,
    KIND_JUMP = 0xFF,
    JUMP_STEP = 6,
    FIRST_TITLE_STEP = 2,
    SECOND_TITLE_STEP = 4,
    TITLE_UPDATE = 0x0006B290u,
    STATE_FRAME = 8u,
    TITLE_FADE_FRAME = 1740,
    TITLE_HOLD_FRAMES = 540
};

void sub_0006B290(void); /* generated title step update */

static bool enabled(void)
{
    static int setting = -1;

    if (setting < 0) {
        const char *value = getenv("RECOMP_ATTRACT_NO_MOVIES");

        setting = value != NULL && strcmp(value, "1") == 0;
    }
    return setting != 0;
}

/* Repoints the jump. Returns false, leaving the feature off, when the table
   does not hold the sequence this was written against. */
static bool prepare(void)
{
    static const uint8_t expected[STEP_COUNT] = {
        KIND_WAIT, KIND_OPENING_MOVIE, KIND_TITLE, KIND_MOVIE, KIND_TITLE, KIND_DEMO, KIND_JUMP
    };
    static int state;
    uint8_t *table;

    if (state != 0) {
        return state > 0;
    }
    state = -1;
    table = recomp_memory(STEP_TABLE, STEP_COUNT * STEP_SIZE);
    for (unsigned step = 0; table != NULL && step < STEP_COUNT; ++step) {
        if (table[step * STEP_SIZE] != expected[step]) {
            table = NULL;
        }
    }
    if (table == NULL || table[JUMP_STEP * STEP_SIZE + STEP_PARAMETER] != FIRST_TITLE_STEP) {
        fprintf(stderr, "[attract] unexpected step table; movies left in\n");
        return false;
    }
    table[JUMP_STEP * STEP_SIZE + STEP_PARAMETER] = SECOND_TITLE_STEP;
    state = 1;
    return true;
}

void recomp_attract_sequence_frame(void)
{
    if (enabled()) {
        prepare();
    }
}

static void title_update(void)
{
    if (enabled() && prepare()) {
        uint32_t *frame = recomp_memory_u32(
            *recomp_memory_u32(recomp_runtime.registers.esp + 4u) + STATE_FRAME);

        if (*frame >= TITLE_HOLD_FRAMES && *frame < TITLE_FADE_FRAME) {
            *frame = TITLE_FADE_FRAME;
        }
    }
    sub_0006B290();
}

/* Seam: title_update adjusts the step's frame counter and then calls the
   generated update, which still owns the title's music and fade. */
RecompFunction recomp_attract_sequence_lookup_manual(uint32_t guest_address)
{
    return guest_address == TITLE_UPDATE ? title_update : NULL;
}
#endif
