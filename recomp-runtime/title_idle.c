#include "title_idle.h"

#ifdef RECOMP_FULL_PROGRAM
#include "cpu_exhibition.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the game does when the title screen is left idle, chosen with
   RECOMP_TITLE_IDLE: unset or "default" for the game's own attract sequence,
   "no-cutscene" for that sequence without its movies, "cpu-exhibition" for
   CPU Exhibition matches (cpu_exhibition.c).

   Left idle, the game walks a table of seven steps at 0x328C14, four bytes
   each: a kind, a pad byte and a 16-bit parameter. Kind 0 waits, kinds 1 and
   5 play a pre-rendered movie, kind 2 holds the title screen, kind 3 runs
   the real-time volleyball demo and 0xFF jumps to the step in its parameter.
   The sequence is wait, movie, title, movie, title, demo, jump to the first
   title. Booting starts it at the second movie (0x000A4920 asks for the
   first step of kind 5), so the loop is movie, title, demo, title.

   The table is ordinary writable data. For "no-cutscene" the jump is
   pointed at the second title instead, which leaves the loop as title, demo.
   The movie still plays once after the logos, and every step change left is
   one the game already makes.

   The title step's update at 0x0006B290 holds the screen for 1800 frames
   and starts its fade at frame 1740. The title scene the game shows after
   START ends a match runs the same step. For "no-cutscene" the frame counter
   in the step state (offset 8 of the block the update is handed) is moved
   to the fade once the title has been up for TITLE_HOLD_FRAMES. For
   "cpu-exhibition" the matches are started just before the fade would
   begin, so the title is idle for as long as the game holds it. */
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
    TITLE_HOLD_FRAMES = 540,
    EXHIBITION_FRAME = TITLE_FADE_FRAME - 2
};

typedef enum {
    IDLE_DEFAULT,
    IDLE_NO_CUTSCENE,
    IDLE_CPU_EXHIBITION
} IdleBehavior;

void sub_0006B290(void); /* generated title step update */

static IdleBehavior behavior(void)
{
    static int setting = -1;

    if (setting < 0) {
        const char *value = getenv("RECOMP_TITLE_IDLE");

        setting = IDLE_DEFAULT;
        if (value != NULL && strcmp(value, "no-cutscene") == 0) {
            setting = IDLE_NO_CUTSCENE;
        } else if (value != NULL && strcmp(value, "cpu-exhibition") == 0) {
            setting = IDLE_CPU_EXHIBITION;
        }
    }
    return (IdleBehavior)setting;
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
        fprintf(stderr, "[title-idle] unexpected step table; movies left in\n");
        return false;
    }
    table[JUMP_STEP * STEP_SIZE + STEP_PARAMETER] = SECOND_TITLE_STEP;
    state = 1;
    return true;
}

void recomp_title_idle_frame(void)
{
    if (behavior() == IDLE_NO_CUTSCENE) {
        prepare();
    }
    recomp_cpu_exhibition_frame();
}

static void title_update(void)
{
    uint32_t *frame = recomp_memory_u32(
        *recomp_memory_u32(recomp_runtime.registers.esp + 4u) + STATE_FRAME);

    if (behavior() == IDLE_NO_CUTSCENE && prepare()) {
        if (*frame >= TITLE_HOLD_FRAMES && *frame < TITLE_FADE_FRAME) {
            *frame = TITLE_FADE_FRAME;
        }
    } else if (behavior() == IDLE_CPU_EXHIBITION && *frame >= EXHIBITION_FRAME) {
        recomp_cpu_exhibition_start();
    }
    sub_0006B290();
}

/* Seam: title_update acts on the step's frame counter and then calls the
   generated update, which still owns the title's music and fade. */
RecompFunction recomp_title_idle_lookup_manual(uint32_t guest_address)
{
    return guest_address == TITLE_UPDATE ? title_update : NULL;
}
#endif
