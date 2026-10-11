/* Synthetic guest memory; no retail data or generated code. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#define RECOMP_FULL_PROGRAM
#include "attract_sequence.c"

RecompRuntime recomp_runtime;
static uint8_t memory[RECOMP_XBOX_RAM_SIZE];
uint8_t *recomp_fast_ram;
static unsigned title_updates;

uint8_t *recomp_memory(uint32_t address, size_t size)
{
    if (address > sizeof memory || size > sizeof memory - address) {
        fprintf(stderr, "Attract memory out of bounds: address=%u size=%zu\n", address, size);
        abort();
    }
    return memory + address;
}
uint32_t *recomp_memory_u32_checked(uint32_t address) { return (uint32_t *)recomp_memory(address, 4); }
void sub_0006B290(void) { ++title_updates; }

enum {
    TEST_STACK = 0x00400000u,
    TEST_STATE = 0x004D9944u
};

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Attract line %d: %s\n", __LINE__, #c); return 1; } } while (0)

/* Runs the title step's update with the frame counter at `frame`. */
static uint32_t title_frame(uint32_t frame)
{
    *recomp_memory_u32(TEST_STATE + STATE_FRAME) = frame;
    recomp_attract_sequence_lookup_manual(TITLE_UPDATE)();
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

#ifdef _WIN32
    _putenv_s("RECOMP_ATTRACT_NO_MOVIES", "1");
#else
    setenv("RECOMP_ATTRACT_NO_MOVIES", "1", 1);
#endif
    recomp_runtime.registers.esp = TEST_STACK;
    *recomp_memory_u32(TEST_STACK + 4u) = TEST_STATE;
    memcpy(recomp_memory(STEP_TABLE, sizeof table), table, sizeof table);
    CHECK(recomp_attract_sequence_lookup_manual(TITLE_UPDATE + 1u) == NULL);

    /* A table that is not the expected sequence is left alone, and so is the
       title's hold. */
    if (argc > 1 && strcmp(argv[1], "--unexpected-table") == 0) {
        memcpy(patched, table, sizeof table);
        patched[3 * STEP_SIZE] = KIND_DEMO;
        memcpy(recomp_memory(STEP_TABLE, sizeof table), patched, sizeof table);
        recomp_attract_sequence_frame();
        recomp_attract_sequence_frame();
        CHECK(memcmp(recomp_memory(STEP_TABLE, sizeof table), patched, sizeof table) == 0);
        CHECK(title_frame(TITLE_HOLD_FRAMES) == TITLE_HOLD_FRAMES);
        CHECK(title_updates == 1u);
        return 0;
    }

    /* Only the jump changes, and it lands on the title that precedes the demo. */
    recomp_attract_sequence_frame();
    memcpy(patched, table, sizeof table);
    patched[JUMP_STEP * STEP_SIZE + STEP_PARAMETER] = SECOND_TITLE_STEP;
    CHECK(memcmp(recomp_memory(STEP_TABLE, sizeof table), patched, sizeof table) == 0);
    recomp_attract_sequence_frame();
    CHECK(memcmp(recomp_memory(STEP_TABLE, sizeof table), patched, sizeof table) == 0);

    /* The title is held, then moved to its fade, which runs untouched. */
    CHECK(title_frame(1u) == 1u);
    CHECK(title_frame(TITLE_HOLD_FRAMES - 1u) == TITLE_HOLD_FRAMES - 1u);
    CHECK(title_frame(TITLE_HOLD_FRAMES) == TITLE_FADE_FRAME);
    CHECK(title_frame(TITLE_FADE_FRAME + 1u) == TITLE_FADE_FRAME + 1u);
    CHECK(title_updates == 4u);
    return 0;
}
