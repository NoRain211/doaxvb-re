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
   3 while a vacation is being chosen and 4 while the prompt under it is
   open, and +0xD is the vacation under the cursor.

   The game's menus are list widgets in an array of 0xD0-byte records from
   0x5C5800 (viewer_menu_adapter.c knows the first): +0x1A the cursor
   (word), +0x5C the item count, +0x60 pointers to the items' texts, and
   +0x88 the phase: 1 opening, 2 open, 4 closing, 5 closed. List 0 is the
   main menu, the pool's, the hotel's and the Casino's; list 1 the row of
   vacations and the island's character select, which has no texts; list 2
   the prompt under a vacation (Continue, Options, Delete a saved game, or
   New Game and Options for an empty one); list 3 the shop's prompts, open
   only while a choice is confirmed (the shop's step tracks them). A
   closed list keeps its items, so the cursor is logged only while the list
   is open.

   On the island the byte at 0x4D72E3 says where the player is: 0 the
   character select of a new vacation (list 1, the girls in the order of
   PICK_ORDER), 17 the opening scene before it, 1 the map,
   3 a shop, 4 the items screen, 5 a place with a menu, 6 the Collection,
   7 a rest at the pool, 8 the Casino (255 off the island). 5 is the pool by
   day and the hotel at night: the byte at 0x41EFEB is the part of the day,
   0 morning to 3 night, and the pool is closed at night. On the map the
   byte at 0x3AF875 is the location the map points at. On the items screen
   the byte at 0x31BD69 is the tab (RT and LT step through Swimsuit,
   Accessory, Item and Present) and the word at 0x31BE04 the cursor in its
   grid of three columns. Making a gift at the hotel shows the same grid on
   its Present tab without changing the place. On the Collection screen the
   byte at 0x31CC7D is the tab (Swimsuit, Accessory, Item) and the word at
   0x31CE0C the cursor over all its pages of 25, five columns a page.

   A shop keeps its screen in the block at 0x3AF874, the context its row
   draw 0x000C8840 is given; the word at +0x18 points to its list state:
   +0x22 the highlighted row of the visible window, +0x24 the entry at the
   window's top (it wraps) and +0x26 the entry count. The entry under the
   cursor is (top + row) % count of shop *(0x3AF8A1), and its item the first
   word of 16-byte entry shop * 15 + entry in the table at *(0x317638).
   The block's byte +0x20 is the shop's step: 1 the list, 2 the Buy/No
   prompt, 3 the Wrap/No prompt that follows Buy. The prompts' cursor is the
   word at 0x5C5A8A, 0 the first button.

   The byte at 0x5C5FED, in the message window's block, is set while a
   conversation is going on.

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
    SAVE_STEP = SAVE_SELECT + 0x10u,
    STEP_VACATIONS = 3,
    LIST_BASE = 0x005C5800u,
    LIST_STRIDE = 0xD0,
    LIST_COUNT = 4,
    LIST_CURSOR = 0x1A,
    LIST_ITEMS = 0x5C,
    LIST_TEXTS = 0x60,
    LIST_PHASE = 0x88,
    LIST_COLUMNS = 0x0D,
    PICK_GIRLS = 8,
    LIST_OPEN = 2,
    LIST_LIMIT = 10,
    ISLAND_PLACE = 0x004D72E3u,
    PLACE_PICK = 0,
    PLACE_OPENING = 17,
    PLACE_MAP = 1,
    PLACE_SHOP = 3,
    PLACE_ITEMS = 4,
    PLACE_MENU = 5,
    PLACE_COLLECTION = 6,
    PLACE_REST = 7,
    PLACE_CASINO = 8,
    DAY_PART = 0x0041EFEBu,
    PART_NIGHT = 3,
    PART_COUNT = 4,
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
    COLLECTION_TAB = 0x0031CC7Du,
    COLLECTION_CURSOR = 0x0031CE0Cu,
    COLLECTION_PAGE = 25,
    DIALOG_SHOWN = 0x005C5FEDu,
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
    case PLACE_PICK: return "pick";
    case PLACE_OPENING: return "opening";
    case PLACE_MAP: return "map";
    case PLACE_SHOP: return "shop";
    case PLACE_ITEMS: return "items";
    case PLACE_COLLECTION: return "collection";
    case PLACE_REST: return "rest";
    case PLACE_CASINO: return "casino";
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

/* The new vacation's character select shows the girls in this order. */
static const char *pick_name(int cursor)
{
    static const char *const names[] = {"Kasumi", "Hitomi", "Helena", "Leifang", "Christie", "Lisa", "Tina",
                                        "Ayane"};

    return cursor >= 0 && cursor < (int)(sizeof names / sizeof names[0]) ? names[cursor] : "?";
}

static const char *part_name(int part)
{
    static const char *const names[PART_COUNT] = {"morning", "daytime", "evening", "night"};

    return part >= 0 && part < PART_COUNT ? names[part] : NULL;
}

static const char *tab_name(int tab)
{
    /* The items screen's tabs, as their titles read. */
    static const char *const names[TAB_COUNT] = {"Swimsuit", "Accessory", "Item", "Present"};

    return tab >= 0 && tab < TAB_COUNT ? names[tab] : "?";
}

/* Copies a guest string, printable characters only; "?" when it is empty. */
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
    if (length == 0u) {
        /* Some items' texts are made as they are drawn, such as the
           Casino's chip price. */
        snprintf(text, size, "?");
    }
    return text;
}

/* While the save select is up and no vacation is chosen yet, the vacation
   under the cursor. The prompt under a chosen vacation is list 2. */
static void log_save_select(bool entered)
{
    static int step, vacation;
    const int now_step = *recomp_memory(SAVE_STEP, 1);
    const int now_vacation = *recomp_memory(SAVE_VACATION, 1);

    if (entered) {
        step = vacation = UNSET;
    }
    if (now_step == STEP_VACATIONS && (now_step != step || now_vacation != vacation)) {
        fprintf(output, "[auto:ui] frame %u: save vacation %d\n", frame, now_vacation + 1);
    }
    step = now_step;
    vacation = now_vacation;
}

/* For each list widget: whether it is open, and while it is its cursor and
   the text of the item under it (a widget without texts, such as the save
   select's row of vacations, gives only its cursor). */
static void log_lists(void)
{
    static int phase[LIST_COUNT], cursor[LIST_COUNT], count[LIST_COUNT];
    static bool started;
    char text[TEXT_LIMIT];

    if (!started) {
        for (unsigned k = 0; k < LIST_COUNT; ++k) {
            phase[k] = cursor[k] = count[k] = UNSET;
        }
        started = true;
    }
    for (unsigned k = 0; k < LIST_COUNT; ++k) {
        const uint32_t base = LIST_BASE + k * LIST_STRIDE;
        const int now = (int)*recomp_memory_u32(base + LIST_PHASE);
        const int now_count = (int)*recomp_memory_u32(base + LIST_ITEMS);
        const int now_cursor = *recomp_memory_u16(base + LIST_CURSOR);

        if ((now == LIST_OPEN) != (phase[k] == LIST_OPEN)) {
            fprintf(output, "[auto:ui] frame %u: list %u %s\n", frame, k, now == LIST_OPEN ? "open" : "closed");
            cursor[k] = count[k] = UNSET;
        }
        phase[k] = now;
        if (phase[k] != LIST_OPEN || now_count < 0 || now_count > LIST_LIMIT ||
            (now_count > 0 && now_cursor >= now_count) || (now_cursor == cursor[k] && now_count == count[k])) {
            continue;
        }
        cursor[k] = now_cursor;
        count[k] = now_count;
        if (now_count == 0) {
            fprintf(output, "[auto:ui] frame %u: list %u cursor %d\n", frame, k, now_cursor);
        } else {
            fprintf(output, "[auto:ui] frame %u: list %u cursor %d of %d: %s\n", frame, k, now_cursor, now_count,
                    guest_text(*recomp_memory_u32(base + LIST_TEXTS + (uint32_t)now_cursor * 4u), text,
                               sizeof text));
        }
    }
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

/* Whether a conversation is going on: its flag comes on with the first
   line and goes off after the last, and a guided step such as the welcome
   tour's walk into the Accessory Shop keeps it on. */
static void log_dialog(void)
{
    static bool shown;
    const bool now = *recomp_memory(DIALOG_SHOWN, 1) != 0u;

    if (now != shown) {
        shown = now;
        fprintf(output, "[auto:ui] frame %u: dialog %s\n", frame, shown ? "open" : "closed");
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
    static int scene = UNSET, screen = UNSET, place = UNSET, part = UNSET, location = UNSET, tab, cursor;
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
        screen = place = part = UNSET;
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

        now = *recomp_memory(DAY_PART, 1);
        if (now != part) {
            const bool night_changed = (now == PART_NIGHT) != (part == PART_NIGHT);

            part = now;
            log_state("day part", now, part_name(now));
            /* The menu place is named by the part of the day. */
            if (night_changed && place == PLACE_MENU) {
                place = UNSET;
            }
        }
        now = *recomp_memory(ISLAND_PLACE, 1);
        if (now != place) {
            place = now;
            entered = true;
            location = tab = cursor = UNSET;
            log_state("island", now,
                      now == PLACE_MENU ? (part == PART_NIGHT ? "hotel" : "pool") : place_name(now));
        }
        now = *recomp_memory(MAP_LOCATION, 1);
        if (place == PLACE_MAP && now != location) {
            location = now;
            fprintf(output, "[auto:ui] frame %u: map location %d %s\n", frame, now, location_name(now));
        }
        if (place == PLACE_ITEMS) {
            const int now_tab = *recomp_memory(ITEMS_TAB, 1);
            const int now_cursor = *recomp_memory_u16(ITEMS_CURSOR);

            if (now_tab != tab || now_cursor != cursor) {
                tab = now_tab;
                cursor = now_cursor;
                fprintf(output, "[auto:ui] frame %u: items tab %d %s cursor %d\n", frame, tab, tab_name(tab),
                        cursor);
            }
        }
        if (place == PLACE_PICK) {
            /* List 1 still holds the save select's row until the character
               select sets it up for its eight girls. */
            const uint32_t row = LIST_BASE + LIST_STRIDE;
            const int now_cursor = *recomp_memory_u16(row + LIST_CURSOR);

            if (*recomp_memory(row + LIST_COLUMNS, 1) == PICK_GIRLS && now_cursor != cursor) {
                cursor = now_cursor;
                fprintf(output, "[auto:ui] frame %u: pick cursor %d %s\n", frame, cursor, pick_name(cursor));
            }
        }
        if (place == PLACE_COLLECTION) {
            const int now_tab = *recomp_memory(COLLECTION_TAB, 1);
            const int now_cursor = *recomp_memory_u16(COLLECTION_CURSOR);

            if (now_tab != tab || now_cursor != cursor) {
                tab = now_tab;
                cursor = now_cursor;
                fprintf(output, "[auto:ui] frame %u: collection tab %d %s page %d cursor %d\n", frame, tab,
                        tab < 3 ? tab_name(tab) : "?", cursor / COLLECTION_PAGE + 1, cursor);
            }
        }
        if (place == PLACE_SHOP) {
            log_shop(entered);
            log_shop_step(entered);
        }
    }
    log_lists();
    log_dialog();
}
#else
void recomp_ui_log_frame(void) {}
#endif
