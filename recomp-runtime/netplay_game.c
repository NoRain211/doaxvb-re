/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_game.h"

#include "input_pulse_source.h"
#include "runtime.h"

#include <string.h>

/* USA executable; see docs/title-idle.md, docs/ui-log.md and
   docs/rollback-netcode.md. */
enum {
    SCENE = 0x005DECE0u,
    SCENE_ASKED = 0x005DED55u,
    SCENE_MATCH = 0,
    SCENE_MENU = 3,
    PAUSED = 0x004D56EFu,
    RESULTS_OPEN = 0x005D89ECu,
    MATCH_OVER = 0x004D6FC8u,
    MENU_SCREEN = 0x009D9D80u,
    /* The menu screen's phase, 1 while it runs, and the main menu's cursor
       (viewer_menu_adapter.c): 1 is EXHIBITION. */
    MENU_PHASE = 0x009D9DA0u,
    MENU_PHASE_RUNNING = 1,
    MAIN_MENU_CURSOR = 0x009D9E0Cu,
    ITEM_EXHIBITION = 1,
    SCREEN_MAIN = 0,
    SCREEN_EXHIBITION_SETUP = 2,
    SCREEN_CHARACTER_SELECT = 4,
    /* List widgets: 0 the main menu, 2 Exhibition's setup. */
    LIST_BASE = 0x005C5800u,
    LIST_STRIDE = 0xD0,
    LIST_CURSOR = 0x1A,
    LIST_ITEMS = 0x5C,
    LIST_TEXTS = 0x60,
    LIST_PHASE = 0x88,
    LIST_OPEN = 2,
    BUTTON_DOWN = 0x0002u,
    /* One frame pressed, then released, so each press is an edge the menus take. */
    PRESS_EVERY = 8u
};

static uint8_t byte(uint32_t address) { return *recomp_memory(address, 1u); }

bool recomp_netplay_game_rally_live(void)
{
    return byte(SCENE) == SCENE_MATCH && byte(SCENE_ASKED) == SCENE_MATCH && byte(PAUSED) == 0u &&
        *recomp_memory_u32(RESULTS_OPEN) == 0u && byte(MATCH_OVER) == 0u;
}

bool recomp_netplay_game_character_select(void)
{
    return byte(SCENE) == SCENE_MENU && (int8_t)byte(MENU_SCREEN) == SCREEN_CHARACTER_SELECT;
}

bool recomp_netplay_game_main_menu(void)
{
    return byte(SCENE) == SCENE_MENU && (int8_t)byte(MENU_SCREEN) == SCREEN_MAIN;
}

bool recomp_netplay_game_exhibition_chosen(void)
{
    return recomp_netplay_game_main_menu() && *recomp_memory_u32(MENU_PHASE) == MENU_PHASE_RUNNING &&
        *recomp_memory_u32(MAIN_MENU_CURSOR) == ITEM_EXHIBITION &&
        *recomp_memory_u32(LIST_BASE + LIST_PHASE) == LIST_OPEN;
}

/* A on the open list's item that reads target, otherwise Down; nothing
   while the list is not open. */
static void choose(unsigned list, const char *target, RecompInputGamepad *pad)
{
    const uint32_t at = LIST_BASE + list * LIST_STRIDE;
    const uint32_t cursor = *recomp_memory_u16(at + LIST_CURSOR);
    if (*recomp_memory_u32(at + LIST_PHASE) != LIST_OPEN || cursor >= *recomp_memory_u32(at + LIST_ITEMS)) return;
    const uint32_t text = *recomp_memory_u32(at + LIST_TEXTS + cursor * 4u);
    const size_t length = strlen(target);
    if (text != 0u && memcmp(recomp_memory(text, length + 1u), target, length + 1u) == 0)
        pad->analog_buttons[RECOMP_INPUT_ANALOG_A] = RECOMP_INPUT_ANALOG_PRESSED;
    else
        pad->buttons = BUTTON_DOWN;
}

bool recomp_netplay_game_walk(uint32_t frame, RecompInputGamepad *pad)
{
    memset(pad, 0, sizeof *pad);
    if (recomp_netplay_game_character_select()) return true;
    if (frame % PRESS_EVERY != 0u) return false;
    const uint8_t scene = byte(SCENE);
    const int8_t screen = (int8_t)byte(MENU_SCREEN);
    if (scene != SCENE_MENU && scene != SCENE_MATCH) pad->buttons = RECOMP_INPUT_BUTTON_START; // movies and title
    else if (scene == SCENE_MENU && screen == SCREEN_MAIN) choose(0u, "EXHIBITION", pad);
    else if (scene == SCENE_MENU && screen == SCREEN_EXHIBITION_SETUP) choose(2u, "Two Players", pad);
    return false;
}
