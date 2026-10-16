#include "cpu_exhibition.h"

#ifdef RECOMP_FULL_PROGRAM
#include "runtime.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* CPU Exhibition: Exhibition matches played by the CPU on every side, one
   after another. title_idle.c starts it when the title screen has been left
   idle, and START ends it.

   The game's top-level loop at 0x000A3820 runs one scene at a time: 0x5DECE0
   is the scene running and each scene's update asks for the next one. The
   title (scene 4) and the attract sequence (scene 2) leave for the main menu
   (scene 3) once START has been pressed. The menu leaves for an Exhibition
   match (scene 0) once character select is confirmed, and a match that ends
   returns to scene 3 once its results menu is answered.

   So the mode needs no input: it stores the same requests the game stores
   for itself, and before each match it fills in character select's four
   choices with a cast of its own and marks every player as the CPU's. */
enum {
    SCENE = 0x005DECE0u,
    SCENE_MATCH = 0,
    SCENE_ATTRACT = 2,
    SCENE_MENU = 3,
    SCENE_TITLE = 4,
    /* Buttons held on any controller; START is the Xbox bit. */
    BUTTONS_HELD = 0x005F3138u,
    BUTTON_START = 0x0010,
    /* The step of the title or attract sequence now showing has run for
       STEP_FRAMES; START sets STEP_LEAVING to the frames left before the
       sequence moves on. */
    STEP_FRAMES = 0x004D994Cu,
    STEP_LEAVING = 0x004D994Au,
    STEP_SETTLE_FRAMES = 60,
    /* The menu under scene 3 is a state machine at 0x9D9D80: one screen at
       a time, each entering, running (MENU_PHASE_RUNNING) and leaving. A
       screen ends the menu by storing an exit code and leaving for no
       screen; once it has left, the machine hands the code to the callback
       the main menu installed. MENU_EXIT_START makes that callback tear the
       menu down, copy character select's choices to the match and ask for
       the scene MENU_MODE names. */
    MENU_LEAVE_FOR_SCREEN = 0x000F2C10u,
    MENU_EXIT = 0x009D9D9Cu,
    MENU_EXIT_START = 0x20,
    MENU_PHASE = 0x009D9DA0u,
    MENU_PHASE_RUNNING = 1,
    MENU_CALLBACK = 0x009D9DB4u,
    MENU_CALLBACK_MAIN = 0x000F3540u,
    MENU_MODE = 0x009D9E0Cu,
    MENU_MODE_EXHIBITION = 1,
    /* Frames the menu is left alone after it opens. */
    MENU_SETTLE_FRAMES = 90,
    /* Character select's choice for each player: +1 girl, +4 the item id of
       her swimsuit. */
    CHOICE = 0x009DAA68u,
    CHOICE_SIZE = 0xB8,
    CHOICE_GIRL = 1,
    CHOICE_SWIMSUIT = 4,
    /* The court comes from COURT_CHOICE, an index into the ten Exhibition
       courts at COURTS, when character select was the last screen, and
       otherwise from the court now loaded, COURT_LOADED[COURT_VIEW]. */
    COURT_CHOICE = 0x009DADD9u,
    COURTS = 0x002A7A40u,
    COURT_SIZE = 6,
    COURT_COUNT = 10,
    COURT_LOADED = 0x004D92F0u,
    COURT_VIEW = 0x004D56E4u,

    /* After a match the results menu (Again, Character Select, Quit) is
       open while RESULTS_OPEN is set and waits for a choice while
       RESULTS_WAIT is above zero. When the wait is over the game stores the
       choice, 0 for none, and sets MATCH_OVER; the match scene then acts on
       RESULTS_CHOICE. */
    RESULTS_OPEN = 0x005D89ECu,
    RESULTS_WAIT = 0x005D89C4u,
    RESULTS_CHOICE = 0x005D8978u,
    RESULTS_CHARACTER_SELECT = 2,
    RESULTS_QUIT = 3,
    MATCH_OVER = 0x004D6FC8u,
    /* Frames the results stay up. */
    RESULTS_FRAMES = 300,

    SLOT_COUNT = 4,
    GIRL_COUNT = 8,
    /* A slot is a person when HUMAN is set; PORT is then their controller.
       PORT_SLOT is the reverse map. A team (slot & 1) with a person in it
       has TEAM_HUMAN set. */
    HUMAN = 0x005DECE4u,
    PORT = 0x005DECE8u,
    PORT_SLOT = 0x005DECF4u,
    TEAM_HUMAN = 0x004D9658u,
    TEAM_SIZE = 9,
    /* A player of the match: +0 played by the CPU, +5 taking part. */
    PLAYER = 0x004D6FF0u,
    PLAYER_SIZE = 0xBC,
    PLAYER_CPU = 0,
    PLAYER_PRESENT = 5,

    CHARACTER_IDS = 0x002A7B00u,
    ITEM_TABLE = 0x003003E8u,
    ITEM_SIZE = 28,
    ITEM_WEAR_MASK = 16,
    ITEM_TYPE_SWIMSUIT = 2,
    SWIMSUIT_END = 0x149
};

/* No screen to go to, and no exit code stored. */
static const uint32_t MENU_NONE = 0xFFFFFFFFu;

/* Private xorshift32 state, so no shared C runtime rand state is touched. */
static uint32_t random_below(uint32_t count)
{
    static uint32_t state;

    if (state == 0u) {
        /* A pinned court is a test run: the same cast every time. */
        state = getenv("RECOMP_CPU_EXHIBITION_COURT") != NULL ? 0x9E3779B9u : (uint32_t)time(NULL) | 1u;
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state % count;
}

/* A swimsuit the girl can wear, chosen evenly from all of them, or 0 when
   the item table does not look as expected. */
static uint16_t random_swimsuit(unsigned girl)
{
    uint16_t wearable[SWIMSUIT_END];
    unsigned count = 0;

    for (uint16_t id = 1u; id < SWIMSUIT_END; ++id) {
        const uint32_t item = *recomp_memory_u32(ITEM_TABLE + id * 4u);
        const uint8_t *source = item != 0u ? recomp_memory(item, ITEM_SIZE) : NULL;
        uint16_t mask;

        if (source == NULL || source[0] != ITEM_TYPE_SWIMSUIT || source[1] != 0u) {
            return 0u;
        }
        memcpy(&mask, source + ITEM_WEAR_MASK, sizeof mask);
        if ((mask & (1u << girl)) != 0u) {
            wearable[count++] = id;
        }
    }
    return count != 0u ? wearable[random_below(count)] : 0u;
}

/* Who the players were before the first match was cast, for leave_mode. */
static struct {
    bool kept;
    uint32_t human;
    uint32_t port;
    uint32_t port_slot;
    uint8_t team_human[2];
} people;

static void keep_people(void)
{
    if (!people.kept) {
        people.kept = true;
        people.human = *recomp_memory_u32(HUMAN);
        people.port = *recomp_memory_u32(PORT);
        people.port_slot = *recomp_memory_u32(PORT_SLOT);
        people.team_human[0] = *recomp_memory(TEAM_HUMAN, 1u);
        people.team_human[1] = *recomp_memory(TEAM_HUMAN + TEAM_SIZE, 1u);
    }
}

/* The menu has been asked to start a cast match that is not running yet. */
static bool match_asked;
/* START came while that match was loading: quit it once it runs. */
static bool quit_pending;

static void quit_match(void)
{
    *recomp_memory_u32(RESULTS_CHOICE) = RESULTS_QUIT;
    *recomp_memory(MATCH_OVER, 1u) = 1u;
}

/* START ends the mode: the players are people again and a match under way
   is left for the title by storing what Quit on its results menu stores. */
static void leave_mode(uint8_t scene)
{
    if (people.kept) {
        people.kept = false;
        *recomp_memory_u32(HUMAN) = people.human;
        *recomp_memory_u32(PORT) = people.port;
        *recomp_memory_u32(PORT_SLOT) = people.port_slot;
        *recomp_memory(TEAM_HUMAN, 1u) = people.team_human[0];
        *recomp_memory(TEAM_HUMAN + TEAM_SIZE, 1u) = people.team_human[1];
    }
    if (scene == SCENE_MATCH) {
        quit_match();
    } else if (scene == SCENE_MENU && match_asked) {
        /* The menu keeps leaving for the cast match, so the scene still
           reads as the menu until the match has loaded. */
        quit_pending = true;
    }
    match_asked = false;
}

/* Four different girls in random swimsuits on a random court, every slot
   for the CPU. */
static bool cast_match(void)
{
    unsigned girls[GIRL_COUNT];
    /* RECOMP_CPU_EXHIBITION_COURT=0..9 pins the court, for repeatable test runs. */
    const char *pinned = getenv("RECOMP_CPU_EXHIBITION_COURT");
    const unsigned court = pinned != NULL && pinned[0] >= '0' && pinned[0] <= '9' && pinned[1] == 0 ?
        (unsigned)(pinned[0] - '0') : random_below(COURT_COUNT);

    for (unsigned i = 0; i < GIRL_COUNT; ++i) {
        girls[i] = i;
    }
    for (unsigned slot = 0; slot < SLOT_COUNT; ++slot) {
        const unsigned pick = slot + random_below(GIRL_COUNT - slot);
        const unsigned girl = girls[pick];
        const uint16_t swimsuit = random_swimsuit(girl);
        uint8_t *player = recomp_memory(PLAYER + slot * PLAYER_SIZE, PLAYER_SIZE);
        uint8_t *choice = recomp_memory(CHOICE + slot * CHOICE_SIZE, CHOICE_SIZE);

        if (swimsuit == 0u) {
            return false;
        }
        girls[pick] = girls[slot];
        girls[slot] = girl;
        player[PLAYER_CPU] = 1u;
        player[PLAYER_PRESENT] = 1u;
        choice[CHOICE_GIRL] = (uint8_t)girl;
        memcpy(choice + CHOICE_SWIMSUIT, &swimsuit, sizeof swimsuit);
    }
    keep_people();
    *recomp_memory_u32(HUMAN) = 0u;
    *recomp_memory_u32(PORT) = 0xFFFFFFFFu;
    *recomp_memory_u32(PORT_SLOT) = 0xFFFFFFFFu;
    *recomp_memory(TEAM_HUMAN, 1u) = 0u;
    *recomp_memory(TEAM_HUMAN + TEAM_SIZE, 1u) = 0u;
    *recomp_memory(COURT_CHOICE, 1u) = (uint8_t)court;
    *recomp_memory(COURT_LOADED + *recomp_memory(COURT_VIEW, 1u), 1u) =
        *recomp_memory(COURTS + court * COURT_SIZE, 1u);
    return true;
}

/* Ends the menu the way character select does once it is confirmed: store
   the exit code and call the game's own "leave for screen" routine with no
   screen to go to. The guest registers belong to whoever called Swap, so
   they are put back afterwards. */
static bool leave_menu(void)
{
    const RecompRegisters registers = recomp_runtime.registers;
    RecompFpuContext fpu;
    bool called;

    recomp_fpu_context_save(&fpu);
    *recomp_memory_u32(MENU_EXIT) = MENU_EXIT_START;
    recomp_runtime.registers.esp -= 8u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = 0u;
    *recomp_memory_u32(recomp_runtime.registers.esp + 4u) = MENU_NONE;
    called = recomp_dispatch(MENU_LEAVE_FOR_SCREEN);
    recomp_runtime.registers = registers;
    recomp_fpu_context_restore(&fpu);
    return called;
}

/* Frames the results have been up, and whether they are being answered. */
static unsigned results_frames;
static bool results_answering;

/* Shows the results for a while, then answers them with Character Select,
   which returns to the menu scene and so to the next match. */
static void leave_results(void)
{
    if (*recomp_memory_u32(RESULTS_OPEN) != 0u &&
        (int32_t)*recomp_memory_u32(RESULTS_WAIT) > 0) {
        if (++results_frames >= RESULTS_FRAMES) {
            *recomp_memory_u32(RESULTS_WAIT) = 0u;
            results_answering = true;
        }
        return;
    }
    results_frames = 0;
    if (results_answering && *recomp_memory(MATCH_OVER, 1u) == 1u) {
        if (*recomp_memory_u32(RESULTS_CHOICE) == 0u) {
            *recomp_memory_u32(RESULTS_CHOICE) = RESULTS_CHARACTER_SELECT;
        }
        results_answering = false;
    }
}

/* Matches are being played. */
static bool active;
/* Set for good when the image is not the expected one. */
static bool unsupported;
static unsigned menu_frames;

void recomp_cpu_exhibition_start(void)
{
    if (!active && !unsupported) {
        active = true;
        quit_pending = false;
        menu_frames = 0;
        results_frames = 0;
        results_answering = false;
    }
}

void recomp_cpu_exhibition_frame(void)
{
    uint8_t scene;

    if (quit_pending) {
        scene = *recomp_memory(SCENE, 1u);
        if (scene == SCENE_MATCH) {
            quit_match();
        }
        if (scene != SCENE_MENU) {
            quit_pending = false;
        }
    }
    if (!active) {
        return;
    }
    scene = *recomp_memory(SCENE, 1u);
    /* START ends the mode, and so does a scene the mode never asks for,
       which only a person can have chosen. */
    if ((*recomp_memory_u32(BUTTONS_HELD) & BUTTON_START) != 0u ||
        (scene != SCENE_MATCH && scene != SCENE_ATTRACT && scene != SCENE_MENU &&
         scene != SCENE_TITLE)) {
        leave_mode(scene);
        active = false;
        return;
    }
    if (scene != SCENE_MENU) {
        menu_frames = 0;
    }
    if (scene == SCENE_TITLE || scene == SCENE_ATTRACT) {
        if (*recomp_memory_u32(STEP_FRAMES) >= STEP_SETTLE_FRAMES &&
            *recomp_memory(STEP_LEAVING, 1u) == 0u) {
            *recomp_memory(STEP_LEAVING, 1u) = 1u;
        }
    } else if (scene == SCENE_MATCH) {
        match_asked = false;
        leave_results();
    } else if (++menu_frames >= MENU_SETTLE_FRAMES &&
        *recomp_memory_u32(MENU_CALLBACK) == MENU_CALLBACK_MAIN &&
        *recomp_memory_u32(MENU_PHASE) == MENU_PHASE_RUNNING &&
        *recomp_memory_u32(MENU_EXIT) == MENU_NONE) {
        *recomp_memory_u32(MENU_MODE) = MENU_MODE_EXHIBITION;
        if (!cast_match() || !leave_menu()) {
            fprintf(stderr, "[cpu-exhibition] unexpected game image; disabled\n");
            leave_mode(scene);
            active = false;
            unsupported = true;
            return;
        }
        match_asked = true;
        menu_frames = 0;
    }
}
#else
void recomp_cpu_exhibition_start(void) {}
void recomp_cpu_exhibition_frame(void) {}
#endif
