/* Synthetic guest memory; no retail data or generated code. */
#define RECOMP_FULL_PROGRAM
#include "cpu_exhibition.c"

#include <stdlib.h>

RecompRuntime recomp_runtime;
static uint8_t memory[RECOMP_XBOX_RAM_SIZE];
uint8_t *recomp_fast_ram;

uint8_t *recomp_memory(uint32_t address, size_t size)
{
    if (address > sizeof memory || size > sizeof memory - address) {
        fprintf(stderr, "CPU Exhibition memory out of bounds: address=%u size=%zu\n", address, size);
        abort();
    }
    return memory + address;
}
uint32_t *recomp_memory_u32_checked(uint32_t address) { return (uint32_t *)recomp_memory(address, 4); }
void recomp_fpu_context_save(RecompFpuContext *context) { (void)context; }
void recomp_fpu_context_restore(const RecompFpuContext *context) { (void)context; }

enum {
    TEST_ITEMS = 0x00400000u,
    TEST_STACK = 0x00600000u,
    TEST_COURT = 0x27
};

static unsigned dispatches;
static uint32_t dispatched_address;
static uint32_t dispatched_argument;
static bool dispatch_result = true;

/* Stands in for the game routine: records the call and spoils a register. */
bool recomp_dispatch(uint32_t guest_address)
{
    ++dispatches;
    dispatched_address = guest_address;
    dispatched_argument = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    recomp_runtime.registers.ebx = 0u;
    return dispatch_result;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CPU Exhibition line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static void run_frames(unsigned count)
{
    while (count-- != 0u) {
        recomp_cpu_exhibition_frame();
    }
}

int main(void)
{
    static const uint8_t characters[GIRL_COUNT] = {1, 5, 7, 12, 13, 14, 15, 20};
    unsigned seen = 0;

    /* Each girl can wear exactly the suits whose id is hers modulo eight. */
    for (uint16_t id = 1u; id < SWIMSUIT_END; ++id) {
        uint8_t *definition = recomp_memory(TEST_ITEMS + id * ITEM_SIZE, ITEM_SIZE);
        const uint16_t wear = (uint16_t)(1u << (id % GIRL_COUNT));

        definition[0] = ITEM_TYPE_SWIMSUIT;
        memcpy(definition + ITEM_WEAR_MASK, &wear, sizeof wear);
        *recomp_memory_u32(ITEM_TABLE + id * 4u) = TEST_ITEMS + id * ITEM_SIZE;
    }
    memcpy(recomp_memory(CHARACTER_IDS, GIRL_COUNT), characters, GIRL_COUNT);
    for (unsigned court = 0; court < COURT_COUNT; ++court) {
        *recomp_memory(COURTS + court * COURT_SIZE, 1u) = (uint8_t)(TEST_COURT + court);
    }
    recomp_runtime.registers.esp = TEST_STACK;
    recomp_runtime.registers.ebx = 0x1234u;

    /* Nothing is done until the mode is started. */
    *recomp_memory(SCENE, 1u) = SCENE_TITLE;
    *recomp_memory_u32(STEP_FRAMES) = STEP_SETTLE_FRAMES;
    run_frames(1);
    CHECK(*recomp_memory(STEP_LEAVING, 1u) == 0u);
    recomp_cpu_exhibition_start();

    /* The title is left alone until its step has run for a while, and a
       step already leaving is not hurried. */
    *recomp_memory(SCENE, 1u) = SCENE_TITLE;
    *recomp_memory_u32(STEP_FRAMES) = STEP_SETTLE_FRAMES - 1u;
    run_frames(1);
    CHECK(*recomp_memory(STEP_LEAVING, 1u) == 0u);
    *recomp_memory_u32(STEP_FRAMES) = STEP_SETTLE_FRAMES;
    run_frames(1);
    CHECK(*recomp_memory(STEP_LEAVING, 1u) == 1u);
    *recomp_memory(STEP_LEAVING, 1u) = 30u;
    run_frames(1);
    CHECK(*recomp_memory(STEP_LEAVING, 1u) == 30u);
    CHECK(dispatches == 0u);

    /* The menu is left alone while it settles, while another menu owns the
       machine, while a screen is changing and while an exit is pending. */
    *recomp_memory(SCENE, 1u) = SCENE_MENU;
    *recomp_memory_u32(HUMAN) = 0x01010101u;
    *recomp_memory_u32(MENU_CALLBACK) = MENU_CALLBACK_MAIN;
    *recomp_memory_u32(MENU_PHASE) = MENU_PHASE_RUNNING;
    *recomp_memory_u32(MENU_EXIT) = MENU_NONE;
    run_frames(MENU_SETTLE_FRAMES - 1u);
    CHECK(dispatches == 0u);
    *recomp_memory_u32(MENU_CALLBACK) = 0u;
    run_frames(1);
    *recomp_memory_u32(MENU_CALLBACK) = MENU_CALLBACK_MAIN;
    *recomp_memory_u32(MENU_PHASE) = 0u;
    run_frames(1);
    *recomp_memory_u32(MENU_PHASE) = MENU_PHASE_RUNNING;
    *recomp_memory_u32(MENU_EXIT) = 0x10u;
    run_frames(1);
    CHECK(dispatches == 0u);
    CHECK(*recomp_memory_u32(HUMAN) == 0x01010101u);
    CHECK(*recomp_memory_u32(MENU_EXIT) == 0x10u);

    /* Then the match is cast and the menu is asked to start it. */
    *recomp_memory_u32(MENU_EXIT) = MENU_NONE;
    *recomp_memory(COURT_VIEW, 1u) = 2u;
    run_frames(1);
    CHECK(dispatches == 1u);
    CHECK(dispatched_address == MENU_LEAVE_FOR_SCREEN);
    CHECK(dispatched_argument == MENU_NONE);
    CHECK(recomp_runtime.registers.esp == TEST_STACK);
    CHECK(recomp_runtime.registers.ebx == 0x1234u);
    CHECK(*recomp_memory_u32(MENU_EXIT) == MENU_EXIT_START);
    CHECK(*recomp_memory_u32(MENU_MODE) == MENU_MODE_EXHIBITION);
    CHECK(*recomp_memory_u32(HUMAN) == 0u);
    CHECK(*recomp_memory_u32(PORT) == 0xFFFFFFFFu);
    CHECK(*recomp_memory_u32(PORT_SLOT) == 0xFFFFFFFFu);
    CHECK(*recomp_memory(TEAM_HUMAN, 1u) == 0u);
    CHECK(*recomp_memory(TEAM_HUMAN + TEAM_SIZE, 1u) == 0u);
    CHECK(*recomp_memory(COURT_CHOICE, 1u) < COURT_COUNT);
    CHECK(*recomp_memory(COURT_LOADED + 2u, 1u) == TEST_COURT + *recomp_memory(COURT_CHOICE, 1u));
    for (unsigned slot = 0; slot < SLOT_COUNT; ++slot) {
        const uint8_t *choice = recomp_memory(CHOICE + slot * CHOICE_SIZE, CHOICE_SIZE);
        const uint8_t girl = choice[CHOICE_GIRL];
        uint16_t swimsuit;

        memcpy(&swimsuit, choice + CHOICE_SWIMSUIT, sizeof swimsuit);
        CHECK(girl < GIRL_COUNT);
        CHECK((seen & (1u << girl)) == 0u);
        seen |= 1u << girl;
        CHECK(swimsuit >= 1u && swimsuit < SWIMSUIT_END);
        CHECK(swimsuit % GIRL_COUNT == girl);
        CHECK(*recomp_memory(PLAYER + slot * PLAYER_SIZE + PLAYER_CPU, 1u) == 1u);
    }

    /* Nothing more is asked while that exit is pending or a match runs, and
       the next menu gets a fresh settle. */
    run_frames(MENU_SETTLE_FRAMES + 1u);
    *recomp_memory(SCENE, 1u) = SCENE_MATCH;
    run_frames(MENU_SETTLE_FRAMES + 1u);
    *recomp_memory(SCENE, 1u) = SCENE_MENU;
    *recomp_memory_u32(MENU_EXIT) = MENU_NONE;
    run_frames(MENU_SETTLE_FRAMES - 1u);
    CHECK(dispatches == 1u);
    run_frames(1);
    CHECK(dispatches == 2u);

    /* A finished match is not answered for the game... */
    *recomp_memory(SCENE, 1u) = SCENE_MATCH;
    *recomp_memory(MATCH_OVER, 1u) = 1u;
    run_frames(RESULTS_FRAMES * 2u);
    CHECK(*recomp_memory_u32(RESULTS_CHOICE) == 0u);
    *recomp_memory(MATCH_OVER, 1u) = 0u;

    /* ...unless its results menu is waiting: that is shown for a while, its
       wait is ended, and the empty choice the game then stores becomes
       Character Select. */
    *recomp_memory_u32(RESULTS_OPEN) = 1u;
    *recomp_memory_u32(RESULTS_WAIT) = 1200u;
    run_frames(RESULTS_FRAMES - 1u);
    CHECK(*recomp_memory_u32(RESULTS_WAIT) == 1200u);
    run_frames(1);
    CHECK(*recomp_memory_u32(RESULTS_WAIT) == 0u);
    run_frames(1);
    CHECK(*recomp_memory_u32(RESULTS_CHOICE) == 0u);
    *recomp_memory(MATCH_OVER, 1u) = 1u;
    run_frames(1);
    CHECK(*recomp_memory_u32(RESULTS_CHOICE) == RESULTS_CHARACTER_SELECT);
    *recomp_memory_u32(RESULTS_OPEN) = 0u;
    *recomp_memory(MATCH_OVER, 1u) = 0u;
    *recomp_memory_u32(RESULTS_CHOICE) = 0u;

    /* START during a match ends the mode: the match is quit for the title,
       the players are people again and nothing more is done. */
    *recomp_memory(SCENE, 1u) = SCENE_MATCH;
    *recomp_memory_u32(HUMAN) = 0u;
    *recomp_memory_u32(BUTTONS_HELD) = 0x0100u;
    run_frames(1);
    CHECK(*recomp_memory(MATCH_OVER, 1u) == 0u);
    *recomp_memory_u32(BUTTONS_HELD) = BUTTON_START;
    run_frames(1);
    CHECK(*recomp_memory_u32(RESULTS_CHOICE) == RESULTS_QUIT);
    CHECK(*recomp_memory(MATCH_OVER, 1u) == 1u);
    CHECK(*recomp_memory_u32(HUMAN) == 0x01010101u);
    *recomp_memory_u32(BUTTONS_HELD) = 0u;
    *recomp_memory(MATCH_OVER, 1u) = 0u;
    *recomp_memory_u32(RESULTS_CHOICE) = 0u;
    *recomp_memory(SCENE, 1u) = SCENE_MENU;
    *recomp_memory_u32(MENU_EXIT) = MENU_NONE;
    run_frames(MENU_SETTLE_FRAMES * 2u);
    CHECK(dispatches == 2u);

    /* Started again, it casts the next match. */
    recomp_cpu_exhibition_start();
    run_frames(MENU_SETTLE_FRAMES);
    CHECK(dispatches == 3u);
    CHECK(*recomp_memory_u32(HUMAN) == 0u);

    /* START while that match is still loading ends the mode too, and the
       match is quit as soon as it runs. */
    *recomp_memory_u32(BUTTONS_HELD) = BUTTON_START;
    run_frames(1);
    CHECK(*recomp_memory_u32(HUMAN) == 0x01010101u);
    CHECK(*recomp_memory(MATCH_OVER, 1u) == 0u);
    *recomp_memory_u32(BUTTONS_HELD) = 0u;
    run_frames(1);
    CHECK(*recomp_memory(MATCH_OVER, 1u) == 0u);
    *recomp_memory(SCENE, 1u) = SCENE_MATCH;
    run_frames(1);
    CHECK(*recomp_memory_u32(RESULTS_CHOICE) == RESULTS_QUIT);
    CHECK(*recomp_memory(MATCH_OVER, 1u) == 1u);
    *recomp_memory(MATCH_OVER, 1u) = 0u;
    *recomp_memory_u32(RESULTS_CHOICE) = 0u;
    run_frames(1);
    CHECK(*recomp_memory(MATCH_OVER, 1u) == 0u);

    /* Started again from the menu, it casts once more. */
    *recomp_memory(SCENE, 1u) = SCENE_MENU;
    *recomp_memory_u32(MENU_EXIT) = MENU_NONE;
    recomp_cpu_exhibition_start();
    run_frames(MENU_SETTLE_FRAMES);
    CHECK(dispatches == 4u);
    CHECK(*recomp_memory_u32(HUMAN) == 0u);

    /* A scene the mode never asks for ends it too. */
    *recomp_memory(SCENE, 1u) = 1u;
    run_frames(1);
    CHECK(*recomp_memory_u32(HUMAN) == 0x01010101u);
    *recomp_memory(SCENE, 1u) = SCENE_MENU;
    *recomp_memory_u32(MENU_EXIT) = MENU_NONE;
    run_frames(MENU_SETTLE_FRAMES * 2u);
    CHECK(dispatches == 4u);

    /* An image without the routine turns the mode off for good. */
    recomp_cpu_exhibition_start();
    dispatch_result = false;
    run_frames(MENU_SETTLE_FRAMES);
    CHECK(dispatches == 5u);
    CHECK(*recomp_memory_u32(HUMAN) == 0x01010101u);
    *recomp_memory_u32(MENU_EXIT) = MENU_NONE;
    recomp_cpu_exhibition_start();
    run_frames(MENU_SETTLE_FRAMES * 2u);
    CHECK(dispatches == 5u);
    return 0;
}
