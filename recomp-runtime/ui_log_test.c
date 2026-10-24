/* Synthetic guest memory; no retail data or generated code. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#define RECOMP_FULL_PROGRAM
#include "ui_log.c"

RecompRuntime recomp_runtime;
static uint8_t memory[RECOMP_XBOX_RAM_SIZE];
uint8_t *recomp_fast_ram;

uint8_t *recomp_memory(uint32_t address, size_t size)
{
    if (address > sizeof memory || size > sizeof memory - address) {
        fprintf(stderr, "UI log memory out of bounds: address=%u size=%zu\n", address, size);
        abort();
    }
    return memory + address;
}
uint32_t *recomp_memory_u32_checked(uint32_t address) { return (uint32_t *)recomp_memory(address, 4); }
uint16_t *recomp_memory_u16(uint32_t address) { return (uint16_t *)recomp_memory(address, 2); }

enum { TEST_TEXT = 0x00500000u, TEST_STATE = 0x00500100u, TEST_TABLE = 0x00510000u };

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "UI log line %d: %s\n", __LINE__, #c); return 1; } } while (0)

/* Runs one frame and returns what it logged. */
static const char *run_frame(void)
{
    static char text[1024];
    size_t length;

    rewind(output);
    recomp_ui_log_frame();
    fflush(output);
    length = (size_t)ftell(output);
    rewind(output);
    length = fread(text, 1, length < sizeof text - 1u ? length : sizeof text - 1u, output);
    text[length] = '\0';
    rewind(output);
    return text;
}

int main(void)
{
#ifdef _WIN32
    _putenv_s("RECOMP_AUTO_LOG", "1");
#else
    setenv("RECOMP_AUTO_LOG", "1", 1);
#endif
    output = tmpfile();
    CHECK(output != NULL);

    /* Nothing before the first scene; then the title by name. */
    CHECK(strcmp(run_frame(), "") == 0);
    *recomp_memory(SCENE, 1) = SCENE_TITLE;
    CHECK(strcmp(run_frame(), "[auto:ui] frame 2: scene title\n") == 0);
    CHECK(strcmp(run_frame(), "") == 0);

    /* The main menu's list: closed lists are quiet, an open one names the
       item under its cursor. */
    *recomp_memory(SCENE, 1) = SCENE_MENU;
    *recomp_memory(MENU_SCREEN, 1) = SCREEN_MAIN;
    *recomp_memory_u32(LIST_PHASE) = 1u;
    *recomp_memory_u32(LIST_COUNT) = 2u;
    *recomp_memory_u32(LIST_TEXTS + 4u) = TEST_TEXT;
    memcpy(recomp_memory(TEST_TEXT, 11), "EXHIBITION", 11);
    CHECK(strcmp(run_frame(), "[auto:ui] frame 4: scene menu\n[auto:ui] frame 4: menu screen main\n") == 0);
    *recomp_memory_u32(LIST_PHASE) = LIST_OPEN;
    *recomp_memory_u16(LIST_CURSOR) = 1u;
    CHECK(strcmp(run_frame(), "[auto:ui] frame 5: list open\n[auto:ui] frame 5: list cursor 1 of 2: EXHIBITION\n") == 0);
    *recomp_memory_u32(LIST_PHASE) = 4u;
    CHECK(strcmp(run_frame(), "[auto:ui] frame 6: list closed\n") == 0);
    *recomp_memory_u32(LIST_PHASE) = 5u;
    CHECK(strcmp(run_frame(), "") == 0);

    /* The save select: the vacation, then the prompt under it. */
    *recomp_memory(MENU_SCREEN, 1) = SCREEN_SAVE_SELECT;
    *recomp_memory(SAVE_STEP, 1) = STEP_VACATIONS;
    *recomp_memory(SAVE_VACATION, 1) = 1u;
    CHECK(strcmp(run_frame(), "[auto:ui] frame 8: menu screen save-select\n[auto:ui] frame 8: save vacation 2\n") == 0);
    *recomp_memory(SAVE_STEP, 1) = STEP_PROMPT;
    CHECK(strcmp(run_frame(), "[auto:ui] frame 9: save prompt cursor 0: Continue\n") == 0);
    *recomp_memory(MENU_SCREEN, 1) = 0xFFu;
    CHECK(strcmp(run_frame(), "[auto:ui] frame 10: menu screen -1\n") == 0);

    /* The island: the map and its location, then the shop. */
    *recomp_memory(SCENE, 1) = SCENE_ISLAND;
    *recomp_memory(ISLAND_PLACE, 1) = PLACE_MAP;
    *recomp_memory(MAP_LOCATION, 1) = 7u;
    CHECK(strcmp(run_frame(),
                 "[auto:ui] frame 11: scene island\n[auto:ui] frame 11: island map\n"
                 "[auto:ui] frame 11: map location 7 Poolside\n") == 0);
    *recomp_memory(MAP_LOCATION, 1) = 8u;
    CHECK(strcmp(run_frame(), "[auto:ui] frame 12: map location 8 ?\n") == 0);
    *recomp_memory(ISLAND_PLACE, 1) = PLACE_SHOP;
    *recomp_memory(SHOP_NUMBER, 1) = 1u;
    *recomp_memory_u32(SHOP_TABLE) = TEST_TABLE;
    *recomp_memory_u32(SHOP_STATE) = TEST_STATE;
    /* Window row 2 below top entry 14 of 15 wraps to entry 1. */
    *recomp_memory_u16(TEST_STATE + 0x22u) = 2u;
    *recomp_memory_u16(TEST_STATE + 0x24u) = 14u;
    *recomp_memory_u16(TEST_STATE + 0x26u) = 15u;
    *recomp_memory_u16(TEST_TABLE + (1u * SHOP_ENTRIES + 1u) * SHOP_ENTRY_SIZE) = 178u;
    *recomp_memory(SHOP_STEP, 1) = STEP_LIST;
    CHECK(strcmp(run_frame(), "[auto:ui] frame 13: island shop\n[auto:ui] frame 13: shop 1 cursor 1 item 178\n"
                              "[auto:ui] frame 13: shop step list\n") == 0);
    CHECK(strcmp(run_frame(), "") == 0);
    /* Buying: the Buy/No prompt, its cursor on No, then Wrap/No. */
    *recomp_memory(SHOP_STEP, 1) = STEP_BUY;
    *recomp_memory_u16(PROMPT_CURSOR) = 1u;
    CHECK(strcmp(run_frame(), "[auto:ui] frame 15: shop step buy\n[auto:ui] frame 15: shop prompt cursor 1: No\n") == 0);
    *recomp_memory(SHOP_STEP, 1) = STEP_WRAP;
    *recomp_memory_u16(PROMPT_CURSOR) = 0u;
    CHECK(strcmp(run_frame(), "[auto:ui] frame 16: shop step wrap\n[auto:ui] frame 16: shop prompt cursor 0: Wrap\n") == 0);
    *recomp_memory(ISLAND_PLACE, 1) = 9u;
    CHECK(strcmp(run_frame(), "[auto:ui] frame 17: island 9\n") == 0);

    /* The items screen: its tab and grid cursor. */
    *recomp_memory(ISLAND_PLACE, 1) = PLACE_ITEMS;
    *recomp_memory(ITEMS_TAB, 1) = 2u;
    *recomp_memory(ITEMS_CURSOR, 1) = 4u;
    CHECK(strcmp(run_frame(), "[auto:ui] frame 18: island items\n[auto:ui] frame 18: items tab 2 Item cursor 4\n") == 0);
    *recomp_memory(ITEMS_TAB, 1) = 4u;
    CHECK(strcmp(run_frame(), "[auto:ui] frame 19: items tab 4 ? cursor 4\n") == 0);

    fclose(output);
    puts("UI log tests passed");
    return 0;
}
