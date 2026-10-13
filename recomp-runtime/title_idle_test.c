/* Synthetic guest memory; no retail data or generated code. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#define RECOMP_FULL_PROGRAM
#include "title_idle.c"

RecompRuntime recomp_runtime;
static uint8_t memory[RECOMP_XBOX_RAM_SIZE];
uint8_t *recomp_fast_ram;
static unsigned title_updates;
static unsigned exhibition_starts;
static unsigned exhibition_frames;

uint8_t *recomp_memory(uint32_t address, size_t size)
{
    if (address > sizeof memory || size > sizeof memory - address) {
        fprintf(stderr, "Title idle memory out of bounds: address=%u size=%zu\n", address, size);
        abort();
    }
    return memory + address;
}
uint32_t *recomp_memory_u32_checked(uint32_t address) { return (uint32_t *)recomp_memory(address, 4); }
void sub_0006B290(void) { ++title_updates; }
static int16_t held_frames;
void sub_000696D0(void)
{
    const uint32_t state = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);

    memcpy(&held_frames, recomp_memory(*recomp_memory_u32(state + SCRIPT_POSITION) + 2u, 2u), 2u);
}
void recomp_cpu_exhibition_start(void) { ++exhibition_starts; }
void recomp_cpu_exhibition_frame(void) { ++exhibition_frames; }

enum {
    TEST_STACK = 0x00400000u,
    TEST_STATE = 0x004D9944u,
    TEST_SCRIPT = 0x0032AF7Au
};

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Title idle line %d: %s\n", __LINE__, #c); return 1; } } while (0)

/* Runs the title step's update with the frame counter at `frame`. */
static uint32_t title_frame(uint32_t frame)
{
    *recomp_memory_u32(TEST_STATE + STATE_FRAME) = frame;
    recomp_title_idle_lookup_manual(TITLE_UPDATE)();
    return *recomp_memory_u32(TEST_STATE + STATE_FRAME);
}

int main(int argc, char **argv)
{
    static const uint8_t table[STEP_COUNT * STEP_SIZE] = {
        KIND_WAIT, 0, 0, 0,
        KIND_OPENING_MOVIE, 0, 0, 0,
        KIND_TITLE, 0, 0, 0,
        KIND_MOVIE, 0, 4, 0,
        KIND_TITLE, 0, 0, 0,
        KIND_DEMO, 0, 0, 0,
        KIND_JUMP, 0, FIRST_TITLE_STEP, 0
    };
    uint8_t patched[sizeof table];

    const char *idle = argc > 1 && strcmp(argv[1], "--cpu-exhibition") == 0
        ? "cpu-exhibition" : "no-cutscene";

#ifdef _WIN32
    _putenv_s("RECOMP_TITLE_IDLE", idle);
#else
    setenv("RECOMP_TITLE_IDLE", idle, 1);
#endif
    recomp_runtime.registers.esp = TEST_STACK;

    /* A demo cut holds for the chosen frames, the script keeps its own 60,
       and another script's hold is left alone. */
    if (argc > 1 && strcmp(argv[1], "--cut-hold") == 0) {
        const int16_t script_hold = DEMO_CUT_HOLD_FRAMES;
        int16_t after;

        CHECK(recomp_title_idle_lookup_manual(DEMO_CUT_HOLD) == NULL);
#ifdef _WIN32
        _putenv_s("RECOMP_DEMO_CUT_HOLD", "12");
#else
        setenv("RECOMP_DEMO_CUT_HOLD", "12", 1);
#endif
        cut_hold_setting = 0;
        memcpy(recomp_memory(TEST_SCRIPT + 2u, 2u), &script_hold, 2u);
        *recomp_memory_u32(DEMO_SCRIPT + SCRIPT_POSITION) = TEST_SCRIPT;
        *recomp_memory_u32(TEST_STACK + 4u) = DEMO_SCRIPT;
        recomp_title_idle_lookup_manual(DEMO_CUT_HOLD)();
        CHECK(held_frames == 12);
        memcpy(&after, recomp_memory(TEST_SCRIPT + 2u, 2u), 2u);
        CHECK(after == DEMO_CUT_HOLD_FRAMES);
        *recomp_memory_u32(TEST_STATE + SCRIPT_POSITION) = TEST_SCRIPT;
        *recomp_memory_u32(TEST_STACK + 4u) = TEST_STATE;
        recomp_title_idle_lookup_manual(DEMO_CUT_HOLD)();
        CHECK(held_frames == DEMO_CUT_HOLD_FRAMES);
        return 0;
    }
    *recomp_memory_u32(TEST_STACK + 4u) = TEST_STATE;
    memcpy(recomp_memory(STEP_TABLE, sizeof table), table, sizeof table);
    CHECK(recomp_title_idle_lookup_manual(TITLE_UPDATE + 1u) == NULL);

    /* CPU Exhibition leaves the table and the title's hold alone, and is
       started once the title has been idle for a while, unless START has already told the title to leave. */
    if (strcmp(idle, "cpu-exhibition") == 0) {
        recomp_title_idle_frame();
        CHECK(memcmp(recomp_memory(STEP_TABLE, sizeof table), table, sizeof table) == 0);
        CHECK(exhibition_frames == 1u);
        CHECK(title_frame(1u) == 1u);
        CHECK(title_frame(TITLE_HOLD_FRAMES - 1u) == TITLE_HOLD_FRAMES - 1u);
        CHECK(exhibition_starts == 0u);
        *recomp_memory(TEST_STATE + STATE_LEAVING, 1u) = 30u;
        CHECK(title_frame(TITLE_HOLD_FRAMES) == TITLE_HOLD_FRAMES);
        CHECK(exhibition_starts == 0u);
        *recomp_memory(TEST_STATE + STATE_LEAVING, 1u) = 0u;
        CHECK(title_frame(TITLE_HOLD_FRAMES) == TITLE_HOLD_FRAMES);
        CHECK(exhibition_starts == 1u);
        CHECK(title_updates == 4u);
        return 0;
    }

    /* A table that is not the expected sequence is left alone, and so is the
       title's hold. */
    if (argc > 1 && strcmp(argv[1], "--unexpected-table") == 0) {
        memcpy(patched, table, sizeof table);
        patched[3 * STEP_SIZE] = KIND_DEMO;
        memcpy(recomp_memory(STEP_TABLE, sizeof table), patched, sizeof table);
        recomp_title_idle_frame();
        recomp_title_idle_frame();
        CHECK(memcmp(recomp_memory(STEP_TABLE, sizeof table), patched, sizeof table) == 0);
        CHECK(title_frame(TITLE_HOLD_FRAMES) == TITLE_HOLD_FRAMES);
        CHECK(title_updates == 1u);
        return 0;
    }

    /* Only the jump changes, and it lands on the title that precedes the demo. */
    recomp_title_idle_frame();
    memcpy(patched, table, sizeof table);
    patched[JUMP_STEP * STEP_SIZE + STEP_PARAMETER] = SECOND_TITLE_STEP;
    CHECK(memcmp(recomp_memory(STEP_TABLE, sizeof table), patched, sizeof table) == 0);
    recomp_title_idle_frame();
    CHECK(memcmp(recomp_memory(STEP_TABLE, sizeof table), patched, sizeof table) == 0);

    /* The title is held, then moved to its fade, which runs untouched. */
    CHECK(title_frame(1u) == 1u);
    CHECK(title_frame(TITLE_HOLD_FRAMES - 1u) == TITLE_HOLD_FRAMES - 1u);
    CHECK(title_frame(TITLE_HOLD_FRAMES) == TITLE_FADE_FRAME);
    CHECK(title_frame(TITLE_FADE_FRAME + 1u) == TITLE_FADE_FRAME + 1u);
    CHECK(title_updates == 4u);
    CHECK(exhibition_starts == 0u);
    return 0;
}
