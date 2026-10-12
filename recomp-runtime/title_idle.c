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
   "cpu-exhibition" the matches are started once the title has been up for
   the same TITLE_HOLD_FRAMES.

   The demo is a script of 16-bit words run from the block at 0x4D9688, whose
   offset 0x14 is the script position. Every scene ends with instruction
   0x31, handled at 0x000696D0: it freezes the picture for the frames in its
   operand, 60 in all six scenes, while the next scene starts beneath it.
   The next scene is already drawn beneath the frozen picture.
   RECOMP_DEMO_CUT_HOLD (1 to 59) shortens that freeze. */
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
    /* Frames left before the step is left; START sets it. */
    STATE_LEAVING = 6u,
    STATE_FRAME = 8u,
    TITLE_FADE_FRAME = 1740,
    TITLE_HOLD_FRAMES = 540,
    DEMO_SCRIPT = 0x004D9688u,
    SCRIPT_POSITION = 0x14u,
    DEMO_CUT_HOLD = 0x000696D0u,
    DEMO_CUT_HOLD_FRAMES = 60
};

typedef enum {
    IDLE_DEFAULT,
    IDLE_NO_CUTSCENE,
    IDLE_CPU_EXHIBITION
} IdleBehavior;

void sub_0006B290(void); /* generated title step update */
void sub_000696D0(void); /* generated demo cut hold */

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
    const uint32_t state = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    uint32_t *frame = recomp_memory_u32(state + STATE_FRAME);

    if (behavior() == IDLE_NO_CUTSCENE && prepare()) {
        if (*frame >= TITLE_HOLD_FRAMES && *frame < TITLE_FADE_FRAME) {
            *frame = TITLE_FADE_FRAME;
        }
    } else if (behavior() == IDLE_CPU_EXHIBITION && *frame >= TITLE_HOLD_FRAMES &&
        *recomp_memory(state + STATE_LEAVING, 1u) == 0u) {
        /* A title already leaving was not left idle: START was pressed. */
        recomp_cpu_exhibition_start();
    }
    sub_0006B290();
}

/* Frames a demo cut holds the last picture; 60 leaves the game's own. */
static int cut_hold_setting;

static int cut_hold_frames(void)
{
    int frames = cut_hold_setting;

    if (frames == 0) {
        const char *value = getenv("RECOMP_DEMO_CUT_HOLD");
        const long parsed = value != NULL ? strtol(value, NULL, 10) : 0;

        frames = parsed >= 1 && parsed < DEMO_CUT_HOLD_FRAMES ? (int)parsed : DEMO_CUT_HOLD_FRAMES;
        cut_hold_setting = frames;
    }
    return frames;
}

static void demo_cut_hold(void)
{
    const uint32_t state = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    uint8_t *operand = recomp_memory(*recomp_memory_u32(state + SCRIPT_POSITION) + 2u, 2u);
    int16_t original;

    memcpy(&original, operand, sizeof original);
    if (state == DEMO_SCRIPT && original > cut_hold_frames()) {
        const int16_t shortened = (int16_t)cut_hold_frames();

        memcpy(operand, &shortened, sizeof shortened);
    }
    sub_000696D0();
    memcpy(operand, &original, sizeof original);
}

/* Seams: title_update acts on the step's frame counter and then calls the
   generated update, which still owns the title's music and fade.
   demo_cut_hold lowers the script operand around the generated handler,
   which still owns the freeze. */
RecompFunction recomp_title_idle_lookup_manual(uint32_t guest_address)
{
    if (guest_address == DEMO_CUT_HOLD) {
        return cut_hold_frames() < DEMO_CUT_HOLD_FRAMES ? demo_cut_hold : NULL;
    }
    return guest_address == TITLE_UPDATE ? title_update : NULL;
}
#endif
