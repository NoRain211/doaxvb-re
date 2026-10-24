#include "ui_log.h"

#include <stdlib.h>
#include <string.h>

static bool setting_on(void)
{
    static int cache = -1;

    if (cache < 0) {
        const char *value = getenv("RECOMP_AUTO_LOG");

        cache = value != NULL && strcmp(value, "1") == 0;
    }
    return cache != 0;
}

bool recomp_ui_log_enabled(void)
{
    return setting_on();
}

#ifdef RECOMP_FULL_PROGRAM
#include "runtime.h"

#include <stdio.h>

/* Screen and cursor log, opt-in with RECOMP_AUTO_LOG=1, for scripts that drive
   the game: each line names the screen the game now shows or where its
   cursor now is, so a driver can wait for it instead of sleeping. Lines
   read "[auto:ui] frame N: ...", frames counted from the first logged one.

   The scene: the state machine at 0x000A3820 runs the scene at 0x5DECE0
   (d3d_frame_adapter.c logs its requests). 4 is the title, 3 the menus
   before a vacation or a match, 1 the island and 0 a match
   (viewer_menu_adapter.c). The scene reads 0 before the first one starts,
   so it is logged from the first scene that is not 0.

   The menus run one screen at a time from the table at 0x2A78B0, 32-byte
   records of seven handlers and the screen's data block; the screen is the
   signed byte at 0x9D9D80 (cpu_exhibition.c), -1 between screens. Screen 0
   is the main menu, 1 the save select, 2 Exhibition's setup and 4 character
   select. The save select's data block is at 0x9DA120: +0x10 is its step,
   3 while a vacation is being chosen and 4 while the prompt under it
   (Continue, Options, Delete a saved game) is open; +0xD is the vacation
   under the cursor and +0xE the prompt's cursor.

   Lists such as the main menu and the pool's menu share one widget at
   0x5C57F8 (viewer_menu_adapter.c): the cursor is the word at 0x5C581A, the
   item count the word at 0x5C585C, and the items' texts are pointed to from
   0x5C5860. The word at 0x5C5888 is its phase: 1 opening, 2 open, 4 closing
   and 5 closed. A closed list keeps its items, so the cursor is logged only
   while the list is open.

   On the island the byte at 0x4D72E3 says where the player is: 1 the map,
   3 a shop, 4 the items screen, 5 the pool (255 off the island). On the map
   the byte at 0x3AF875 is the location the map points at. On the items
   screen the byte at 0x31BD69 is the tab (RT and LT step through Swimsuit,
   Accessory, Item and Present) and the byte at 0x31BE04 the cursor in its
   grid of three columns.

   A shop keeps its screen in the block at 0x3AF874, the context its row
   draw 0x000C8840 is given; the word at +0x18 points to its list state:
   +0x22 the highlighted row of the visible window, +0x24 the entry at the
   window's top (it wraps) and +0x26 the entry count. The entry under the
   cursor is (top + row) % count of shop *(0x3AF8A1), and its item the first
   word of 16-byte entry shop * 15 + entry in the table at *(0x317638).
   The block's byte +0x20 is the shop's step: 1 the list, 2 the Buy/No
   prompt, 3 the Wrap/No prompt that follows Buy. The prompts' cursor is the
   word at 0x5C5A8A, 0 the first button.

   These were found by comparing RAM dumps taken at
   each place and cursor position, each checked against a screenshot.

   Guest memory is never written. */
enum {
    SCENE = 0x005DECE0u,
    SCENE_MATCH = 0,
    SCENE_ISLAND = 1,
    SCENE_MENU = 3,
    SCENE_TITLE = 4,
    MENU_SCREEN = 0x009D9D80u,
    SCREEN_MAIN = 0,
    SCREEN_SAVE_SELECT = 1,
    SCREEN_EXHIBITION_SETUP = 2,
    SCREEN_CHARACTER_SELECT = 4,
    SAVE_SELECT = 0x009DA120u,
    SAVE_VACATION = SAVE_SELECT + 0xDu,
    SAVE_PROMPT_CURSOR = SAVE_SELECT + 0xEu,
    SAVE_STEP = SAVE_SELECT + 0x10u,
    STEP_VACATIONS = 3,
    STEP_PROMPT = 4,
    PROMPT_COUNT = 3,
    LIST_CURSOR = 0x005C581Au,
    LIST_COUNT = 0x005C585Cu,
    LIST_TEXTS = 0x005C5860u,
    LIST_PHASE = 0x005C5888u,
    LIST_OPEN = 2,
    LIST_LIMIT = 10,
    ISLAND_PLACE = 0x004D72E3u,
    PLACE_MAP = 1,
    PLACE_SHOP = 3,
    PLACE_ITEMS = 4,
    PLACE_POOL = 5,
    MAP_LOCATION = 0x003AF875u,
    SHOP_STATE = 0x003AF874u + 0x18u,
    SHOP_NUMBER = 0x003AF8A1u,
    SHOP_TABLE = 0x00317638u,
    SHOP_ENTRIES = 15,
    SHOP_ENTRY_SIZE = 16,
    SHOP_STEP = 0x003AF874u + 0x20u,
    STEP_LIST = 1,
    STEP_BUY = 2,
    STEP_WRAP = 3,
    PROMPT_CURSOR = 0x005C5A8Au,
    ITEMS_TAB = 0x0031BD69u,
    ITEMS_CURSOR = 0x0031BE04u,
    TAB_COUNT = 4,
    LOCATION_COUNT = 10,
    TEXT_LIMIT = 48,
    UNSET = -1000
};

static unsigned frame;
static FILE *output;

static const char *scene_name(int scene)
{
    switch (scene) {
    case SCENE_MATCH: return "match";
    case SCENE_ISLAND: return "island";
    case SCENE_MENU: return "menu";
    case SCENE_TITLE: return "title";
    default: return NULL;
    }
}

static const char *screen_name(int screen)
{
    switch (screen) {
    case SCREEN_MAIN: return "main";
    case SCREEN_SAVE_SELECT: return "save-select";
    case SCREEN_EXHIBITION_SETUP: return "exhibition-setup";
    case SCREEN_CHARACTER_SELECT: return "character-select";
    default: return NULL;
    }
}

static const char *place_name(int place)
{
    switch (place) {
    case PLACE_MAP: return "map";
    case PLACE_SHOP: return "shop";
    case PLACE_ITEMS: return "items";
    case PLACE_POOL: return "pool";
    default: return NULL;
    }
}

static const char *location_name(int location)
{
    /* Seen on the map: Right steps up through them from Niki Beach and
       wraps. 8 was skipped on a morning and is not named. */
    static const char *const names[LOCATION_COUNT] = {
        "Niki Beach", "Private Beach", "Jungle", "Bass Island", "Accessory Shop",
        "Sports Shop", "Zack of All Trades", "Poolside", NULL, "Radio Station"
    };

    return location >= 0 && location < LOCATION_COUNT && names[location] != NULL ? names[location] : "?";
}

/* "<what> <name>", or "<what> <number>" when the value has no name. */
static void log_state(const char *what, int value, const char *name)
{
    if (name != NULL) {
        fprintf(output, "[auto:ui] frame %u: %s %s\n", frame, what, name);
    } else {
        fprintf(output, "[auto:ui] frame %u: %s %d\n", frame, what, value);
    }
}

static const char *tab_name(int tab)
{
    /* The items screen's tabs, as their titles read. */
    static const char *const names[TAB_COUNT] = {"Swimsuit", "Accessory", "Item", "Present"};

    return tab >= 0 && tab < TAB_COUNT ? names[tab] : "?";
}

/* Copies a guest string, printable characters only. */
static const char *guest_text(uint32_t at, char *text, size_t size)
{
    size_t length = 0;

    if (at == 0u) {
        snprintf(text, size, "?");
        return text;
    }
    for (; length + 1u < size; ++at) {
        const unsigned char c = *recomp_memory(at, 1);

        if (c == '\0') {
            break;
        }
        text[length++] = c >= 0x20u && c < 0x7Fu ? (char)c : '.';
    }
    text[length] = '\0';
    return text;
}

/* While the save select is up: the vacation under the cursor, or the
   prompt's cursor once a vacation is chosen. */
static void log_save_select(bool entered)
{
    static const char *const prompt[PROMPT_COUNT] = {"Continue", "Options", "Delete a saved game"};
    static int step, vacation, cursor;
    const int now_step = *recomp_memory(SAVE_STEP, 1);
    const int now_vacation = *recomp_memory(SAVE_VACATION, 1);
    const int now_cursor = *recomp_memory(SAVE_PROMPT_CURSOR, 1);

    if (entered) {
        step = vacation = cursor = UNSET;
    }
    if (now_step == STEP_VACATIONS && (now_step != step || now_vacation != vacation)) {
        fprintf(output, "[auto:ui] frame %u: save vacation %d\n", frame, now_vacation + 1);
    } else if (now_step == STEP_PROMPT && (now_step != step || now_cursor != cursor)) {
        fprintf(output, "[auto:ui] frame %u: save prompt cursor %d: %s\n", frame, now_cursor,
                now_cursor >= 0 && now_cursor < PROMPT_COUNT ? prompt[now_cursor] : "?");
    }
    step = now_step;
    vacation = now_vacation;
    cursor = now_cursor;
}

/* Whether the list widget is open, and while it is its cursor and the text
   of the item under it. */
static void log_list(void)
{
    static int phase = UNSET, cursor = UNSET, count = UNSET;
    const int now = (int)*recomp_memory_u32(LIST_PHASE);
    const int open_count = (int)*recomp_memory_u32(LIST_COUNT);
    const int open_cursor = *recomp_memory_u16(LIST_CURSOR);
    char text[TEXT_LIMIT];

    if ((now == LIST_OPEN) != (phase == LIST_OPEN)) {
        fprintf(output, "[auto:ui] frame %u: list %s\n", frame, now == LIST_OPEN ? "open" : "closed");
        cursor = count = UNSET;
    }
    phase = now;
    if (phase != LIST_OPEN || open_count <= 0 || open_count > LIST_LIMIT || open_cursor >= open_count ||
        (open_cursor == cursor && open_count == count)) {
        return;
    }
    cursor = open_cursor;
    count = open_count;
    fprintf(output, "[auto:ui] frame %u: list cursor %d of %d: %s\n", frame, cursor, count,
            guest_text(*recomp_memory_u32(LIST_TEXTS + (uint32_t)cursor * 4u), text, sizeof text));
}

static const char *shop_step_name(int step)
{
    switch (step) {
    case STEP_LIST: return "list";
    case STEP_BUY: return "buy";
    case STEP_WRAP: return "wrap";
    default: return NULL;
    }
}

/* The shop's step and the cursor of its prompts. */
static void log_shop_step(bool entered)
{
    static int step, cursor;
    const int now = *recomp_memory(SHOP_STEP, 1);
    const int now_cursor = *recomp_memory_u16(PROMPT_CURSOR);
    const bool prompt = now == STEP_BUY || now == STEP_WRAP;

    if (entered) {
        step = cursor = UNSET;
    }
    if (now != step) {
        step = now;
        cursor = UNSET;
        log_state("shop step", now, shop_step_name(now));
    }
    if (prompt && now_cursor != cursor && now_cursor >= 0 && now_cursor < 2) {
        cursor = now_cursor;
        fprintf(output, "[auto:ui] frame %u: shop prompt cursor %d: %s\n", frame, cursor,
                cursor == 1 ? "No" : step == STEP_BUY ? "Buy" : "Wrap");
    }
}

/* The shop's cursor and the item under it, once the shop's list is set up. */
static void log_shop(bool entered)
{
    static int shop, cursor;
    const int number = (int8_t)*recomp_memory(SHOP_NUMBER, 1);
    const uint32_t state = *recomp_memory_u32(SHOP_STATE);
    const uint32_t table = *recomp_memory_u32(SHOP_TABLE);
    int count, now;

    if (entered) {
        shop = cursor = UNSET;
    }
    if (number < 0 || state == 0u || table == 0u) {
        return;
    }
    count = *recomp_memory_u16(state + 0x26u);
    if (count <= 0 || count > SHOP_ENTRIES) {
        return;
    }
    now = (*recomp_memory_u16(state + 0x24u) + *recomp_memory_u16(state + 0x22u)) % count;
    if (number == shop && now == cursor) {
        return;
    }
    shop = number;
    cursor = now;
    fprintf(output, "[auto:ui] frame %u: shop %d cursor %d item %u\n", frame, shop, cursor,
            *recomp_memory_u16(table + (uint32_t)(shop * SHOP_ENTRIES + cursor) * SHOP_ENTRY_SIZE));
}

void recomp_ui_log_frame(void)
{
    static bool started;
    static int scene = UNSET, screen = UNSET, place = UNSET, location = UNSET, tab, cursor;
    int now;

    if (!setting_on()) {
        return;
    }
    if (output == NULL) {
        output = stderr;
    }
    ++frame;
    now = *recomp_memory(SCENE, 1);
    started = started || now != SCENE_MATCH;
    if (!started) {
        return;
    }
    if (now != scene) {
        scene = now;
        screen = place = UNSET;
        log_state("scene", now, scene_name(now));
    }
    if (scene == SCENE_MENU) {
        bool entered = false;

        now = (int8_t)*recomp_memory(MENU_SCREEN, 1);
        if (now != screen) {
            screen = now;
            entered = true;
            log_state("menu screen", now, screen_name(now));
        }
        if (screen == SCREEN_SAVE_SELECT) {
            log_save_select(entered);
        }
    }
    if (scene == SCENE_ISLAND) {
        bool entered = false;

        now = *recomp_memory(ISLAND_PLACE, 1);
        if (now != place) {
            place = now;
            entered = true;
            location = tab = cursor = UNSET;
            log_state("island", now, place_name(now));
        }
        now = *recomp_memory(MAP_LOCATION, 1);
        if (place == PLACE_MAP && now != location) {
            location = now;
            fprintf(output, "[auto:ui] frame %u: map location %d %s\n", frame, now, location_name(now));
        }
        if (place == PLACE_ITEMS) {
            const int now_tab = *recomp_memory(ITEMS_TAB, 1);
            const int now_cursor = *recomp_memory(ITEMS_CURSOR, 1);

            if (now_tab != tab || now_cursor != cursor) {
                tab = now_tab;
                cursor = now_cursor;
                fprintf(output, "[auto:ui] frame %u: items tab %d %s cursor %d\n", frame, tab, tab_name(tab),
                        cursor);
            }
        }
        if (place == PLACE_SHOP) {
            log_shop(entered);
            log_shop_step(entered);
        }
    }
    log_list();
}
#else
void recomp_ui_log_frame(void) {}
#endif
