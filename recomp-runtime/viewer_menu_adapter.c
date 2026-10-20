#include "viewer_menu_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RECOMP_FULL_PROGRAM
#include "kernel_abi.h"
#include "actor_pose_adapter.h"
#include "suit_switch_adapter.h"
#include "free_camera_adapter.h"
#include "scene_freeze_adapter.h"
#include "d3d_draw_adapter.h"
#include "d3d_frame_adapter.h"
#include "input_host_win32.h"
#include "overlay_text.h"
#include "viewer_scene.h"

/* MODEL VIEWER: a third item in the main menu, under GO TO ZACK ISLAND and
   EXHIBITION, shown when RECOMP_FREE_CAMERA is set.

   The menu scene runs one screen at a time from the table at 0x2A78B0,
   32-byte records of seven handlers and a data block; the main menu is
   screen 0. Its setup, 0x000F3B30, waits for the menu's resources, creates
   the list widget at 0x5C57F8 and fills its items: for each string number at
   0x2A7A90 it appends the string's text, for the language in 0x3A4C45, to
   the list of text pointers at 0x5C5860, counting them at 0x5C585C (ten at
   most). The widget draws and moves its cursor over however many items the
   list holds, in the game's own font and button frames. After the setup the
   viewer appends one more item, its text in a small pool block.

   The screen's update, 0x000F3E40, keeps the cursor in the screen's data
   block at 0x9D9E0C. Confirming item 0 goes to screen 1 and item 1, Exhibition,
   to screen 2, character select's way in; any other item does nothing. When
   item 2 is confirmed the viewer runs the update as if it were item 1, so the
   game itself plays the sound and leaves for Exhibition, and notes that the
   viewer was asked for. The cursor stays on Exhibition: the menu hands its
   value on as the mode to start, and 2 would start a vacation. Exhibition's
   own screens choose the girl, her swimsuit, the other players and the court.

   Only Player 1's pick matters to the viewer, so once it is made the viewer
   confirms the rest itself: character select keeps its state in its data
   block at 0x9DAA48: +0 the cursor in the row of girls, +2 the number of
   players whose picks are done (0 to 3) and +8 the step, 4 once the court is
   being chosen. While the menu is on character select
   (screen 4), past the first pick and short of the court, the viewer presses
   A on port 0 every AUTO_PICK_POLLS polls, so the game accepts the girl,
   swimsuit and attitude it offers for the partner and both opponents. The
   court is left to the player. Only Player 1's column is shown meanwhile:
   character select draws four columns of 2D quads at fixed places in its
   720 x 480 screen, the first at x 56 to 202, and the viewer has the draw
   adapter leave out every quad wholly inside the other three columns
   (PICKING_MASK). Once the court is being chosen its grid fills the lower
   half, so only the columns' portraits above it are left out (COURT_MASK).
   While the others pick, the row of girls is left out as well (OTHERS_MASK).
   It confirms Exhibition's first screen
   (screen 2: One Player, the match length, controller settings) the same
   way, on One Player.

   The match then loads as usual, but its opening is never seen: from its
   first frame the other three players are hidden and the picture holds
   (RECOMP_PRESENT_HOLD_VIEWER), as it does over Exhibition's first screen. Once it has run for SETTLE_FRAMES the
   viewer takes it over: the free camera comes on close to the first player,
   the scene keeps running, and the other three players are hidden. Putting the
   free camera away ends the viewer: the match is quit the way its results
   menu's Quit does it (0x5D8978 = 3, then 0x4D6FC8 = 1), and the players
   are shown again once the match scene has gone. Quit leads to the title,
   which the viewer moves on to the main menu the way START would: once the
   title step has run STEP_SETTLE_FRAMES (0x4D994C), it sets the step's
   leaving count at 0x4D994A, as CPU Exhibition does. */
enum {
    MAIN_MENU_SETUP = 0x000F3B30u,
    MAIN_MENU_UPDATE = 0x000F3E40u,
    MENU_PHASE = 0x009D9DA0u,
    MENU_PHASE_RUNNING = 1,
    MAIN_MENU_CURSOR = 0x009D9E0Cu,
    ITEM_EXHIBITION = 1,
    ITEM_VIEWER = 2,
    ITEM_TEXTS = 0x005C5860u,
    ITEM_COUNT = 0x005C585Cu,
    ITEM_LIMIT = 10,
    /* Controllers the menu listens to: 0x14-byte records of a port and an
       enabled flag; each port's pressed buttons at 0x4D9548 + port * 0x2C. */
    MENU_PORTS = 0x003AF1D0u,
    MENU_PORTS_END = 0x003AF220u,
    MENU_PORT_SIZE = 0x14u,
    PORT_ACTIVE = 0x005DECE4u,
    PORT_PRESSED = 0x004D9548u,
    PORT_PRESSED_SIZE = 0x2Cu,
    PRESSED_CONFIRM = 0x00010010u,
    /* The list widget: 0x0008D800 makes it, from EAX (its x), ECX and the
       first argument (its rows), the second (its y) and the third. */
    LIST_CREATE = 0x0008D800u,
    LIST_X = 0x168u,
    LIST_Y = 0x140u,
    LIST_FRAME = 0x005C5800u,
    LIST_STYLE = 0x005C5812u,
    LIST_CURSOR = 0x005C581Au,
    LIST_SCROLL = 0x005C581Cu,
    LIST_SHOWN = 0x005C58A2u,
    LIST_STATE_A = 0x005C57F0u,
    LIST_STATE_B = 0x005C5888u,
    LIST_STATE_C = 0x005C5894u,
    LIST_STATE_D = 0x005C5890u,
    LIST_STATE_E = 0x005C5896u,
    SCENE = 0x005DECE0u,
    SCENE_MATCH = 0,
    RESULTS_CHOICE = 0x005D8978u,
    RESULTS_QUIT = 3,
    MATCH_OVER = 0x004D6FC8u,
    SETTLE_FRAMES = 240,
    MENU_SCREEN = 0x009D9D80u,
    SCREEN_EXHIBITION_SETUP = 2,
    SCREEN_CHARACTER_SELECT = 4,
    /* Exhibition setup's cursor, a byte in its data block; 0 is One Player. */
    SETUP_CURSOR = 0x009D9F6Du,
    PICKS = 0x009DAA48u,
    PICKS_DONE = 0x2u,
    BALL_MODEL = 0x004D9440u,
    BALL_HEADERS = 0x1000u,
    PICK_STEP = 0x8u,
    PICK_STEP_COURT = 4,
    AUTO_PICK_POLLS = 12,
    ANALOG_A = 0,
    ANALOG_LTRIG = 6,
    ANALOG_RTRIG = 7,
    ANALOG_PRESSED = 0x20,
    COURTS = 0x002A7A40u,
    COURT_SIZE = 6,
    SCENE_MENU = 3,
    SCENE_TITLE = 4,
    STEP_FRAMES = 0x004D994Cu,
    STEP_LEAVING = 0x004D994Au,
    STEP_SETTLE_FRAMES = 60,
    TITLE_HINT_FRAMES = 300,
    FIRST_PLAYER = 0,
    OTHER_PLAYERS = 0xEu,
};

static const char LABEL[] = "MODEL VIEWER";

void sub_000F3B30(void);
void sub_000F3E40(void);

static int enabled = -1;
static uint32_t label;
static bool requested;

static enum {
    VIEWER_IDLE,
    VIEWER_WAITING,
    VIEWER_VIEWING,
    VIEWER_LEAVING,
} session;
static uint32_t match_frames;

static const float PICKING_MASK[1][4] = {{200.0f, 60.0f, 720.0f, 362.0f}};
static const float COURT_MASK[1][4] = {{200.0f, 60.0f, 720.0f, 250.0f}};
/* Once Player 1 has picked, the row of girls goes too, so the girls the
   others take do not leave it one by one. */
static const float OTHERS_MASK[2][4] = {{200.0f, 60.0f, 720.0f, 362.0f}, {0.0f, 330.0f, 720.0f, 480.0f}};

/* Shows only Player 1's column of character select while the viewer waits. */
/* Time of day. Court select's tiles pick a place at a time; the viewer's
   time setting can swap the court's stage for its place's stage at another
   time (viewer_scene.c). While court select is up, [ and ] or the triggers
   change the setting and the court table at COURTS, six bytes a court with
   the stage first, holds the swapped stages, so the match loads the chosen
   one. The table is put back when the viewer is done. */
static RecompViewerTime chosen_time;
static void choose_time(RecompInputGamepad *gamepad);
static uint8_t own_stages[RECOMP_VIEWER_COURTS];
static bool table_changed;
static bool time_back_held, time_next_held;

static void restore_courts(void)
{
    if (!table_changed) return;
    for (uint32_t court = 0u; court < RECOMP_VIEWER_COURTS; ++court) {
        *recomp_memory(COURTS + court * COURT_SIZE, 1u) = own_stages[court];
    }
    table_changed = false;
}

static void apply_time(void)
{
    if (!table_changed) {
        for (uint32_t court = 0u; court < RECOMP_VIEWER_COURTS; ++court) {
            own_stages[court] = *recomp_memory(COURTS + court * COURT_SIZE, 1u);
        }
        table_changed = true;
    }
    for (uint32_t court = 0u; court < RECOMP_VIEWER_COURTS; ++court) {
        int stage = recomp_viewer_court_stage(court, chosen_time, own_stages[court]);
        if (stage >= 0) *recomp_memory(COURTS + court * COURT_SIZE, 1u) = (uint8_t)stage;
    }
}

/* The match ball's model is the one the loader keeps at BALL_MODEL; its
   vertex buffer objects lie in the first BALL_HEADERS bytes. */
static void hide_ball(bool hide)
{
    uint32_t model = *recomp_memory_u32(BALL_MODEL);
    if (hide && model != 0u && model != 0xFFFFFFFFu) {
        recomp_d3d_draw_set_hidden_buffers(model, model + BALL_HEADERS);
    } else {
        recomp_d3d_draw_set_hidden_buffers(0u, 0u);
    }
}

static bool choosing_court(void)
{
    return session == VIEWER_WAITING && *recomp_memory(SCENE, 1u) == SCENE_MENU &&
        *recomp_memory(MENU_SCREEN, 1u) == SCREEN_CHARACTER_SELECT &&
        *recomp_memory(PICKS + PICK_STEP, 1u) == PICK_STEP_COURT;
}

static void fill_time_panel(RecompOverlayText *text)
{
    recomp_overlay_clear(text);
    recomp_overlay_set_title(text, "MODEL VIEWER");
    recomp_overlay_add_line(text, "Time of day: %s", recomp_viewer_time_name(chosen_time));
    recomp_overlay_set_hint(text, "[ ] or LT RT: time of day");
}

/* For the first TITLE_HINT_FRAMES of each visit to the title screen, a
   panel says how to reach the free camera and the viewer. True when it is
   shown. */
static bool fill_title_hint(RecompOverlayText *text)
{
    static uint32_t title_frames;
    if (*recomp_memory(SCENE, 1u) != SCENE_TITLE || recomp_free_camera_active()) {
        if (*recomp_memory(SCENE, 1u) != SCENE_TITLE) title_frames = 0u;
        return false;
    }
    if (title_frames >= TITLE_HINT_FRAMES) return false;
    ++title_frames;
    recomp_overlay_clear(text);
    recomp_overlay_set_title(text, "FREE CAMERA");
    recomp_overlay_add_line(text, "F1, or click the right stick: free camera");
    recomp_overlay_add_line(text, "MODEL VIEWER is in the main menu");
    return true;
}

static void mask_character_select(void)
{
    static int shown = -1;
    int mask = 0;
    if (session == VIEWER_WAITING && *recomp_memory(SCENE, 1u) == SCENE_MENU &&
        *recomp_memory(MENU_SCREEN, 1u) == SCREEN_CHARACTER_SELECT) {
        mask = *recomp_memory(PICKS + PICK_STEP, 1u) == PICK_STEP_COURT ? 2 :
            *recomp_memory(PICKS + PICKS_DONE, 1u) != 0u ? 3 : 1;
    }
    if (mask == shown) return;
    shown = mask;
    if (mask == 1) recomp_d3d_draw_set_screen_mask(PICKING_MASK, 1u);
    else if (mask == 2) recomp_d3d_draw_set_screen_mask(COURT_MASK, 1u);
    else if (mask == 3) recomp_d3d_draw_set_screen_mask(OTHERS_MASK, 2u);
    else recomp_d3d_draw_set_screen_mask(NULL, 0u);
}

static bool viewer_enabled(void)
{
    if (enabled < 0) {
        const char *value = getenv("RECOMP_FREE_CAMERA");
        enabled = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    }
    return enabled != 0;
}

bool recomp_viewer_menu_requested(void)
{
    return requested;
}

void recomp_viewer_menu_clear(void)
{
    requested = false;
}

/* Calls a cdecl guest function with three arguments and the given EAX, ECX
   and EDX, leaving every register as it was. */
static bool call_guest(uint32_t address, uint32_t eax, uint32_t ecx, uint32_t edx,
    uint32_t a1, uint32_t a2, uint32_t a3)
{
    const RecompRegisters registers = recomp_runtime.registers;
    RecompFpuContext fpu;
    recomp_fpu_context_save(&fpu);
    recomp_runtime.registers.esp -= 16u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = 0u;
    *recomp_memory_u32(recomp_runtime.registers.esp + 4u) = a1;
    *recomp_memory_u32(recomp_runtime.registers.esp + 8u) = a2;
    *recomp_memory_u32(recomp_runtime.registers.esp + 12u) = a3;
    recomp_runtime.registers.eax = eax;
    recomp_runtime.registers.ecx = ecx;
    recomp_runtime.registers.edx = edx;
    bool called = recomp_dispatch(address);
    recomp_runtime.registers = registers;
    recomp_fpu_context_restore(&fpu);
    return called;
}

/* The setup made a two-row list; make it again with a row for the viewer
   and fill it the way the setup fills its own. */
static void add_item(void)
{
    uint32_t count = *recomp_memory_u32(ITEM_COUNT);
    if (count != 2u) return;
    uint32_t texts[3] = {*recomp_memory_u32(ITEM_TEXTS), *recomp_memory_u32(ITEM_TEXTS + 4u), 0u};
    uint32_t cursor = *recomp_memory_u32(MAIN_MENU_CURSOR);
    if (label == 0u) {
        label = recomp_kernel_allocate_pool(sizeof LABEL);
        if (label == 0u) return;
        recomp_guest_store(label, LABEL, sizeof LABEL);
    }
    texts[2] = label;

    if (!call_guest(LIST_CREATE, LIST_X, 3u, 0u, 3u, LIST_Y, 1u)) return;
    uint32_t frame = *recomp_memory_u32(LIST_FRAME);
    if (frame != 0u) *recomp_memory_u16(frame + 0xAu) = 0x28u;
    *recomp_memory_u16(LIST_STYLE) = 0x280u;
    *recomp_memory(LIST_SHOWN, 1u) = 1u;
    for (uint32_t i = 0u; i < 3u; ++i) *recomp_memory_u32(ITEM_TEXTS + i * 4u) = texts[i];
    *recomp_memory_u32(ITEM_COUNT) = 3u;
    *recomp_memory_u16(LIST_SCROLL) = 0u;
    frame = *recomp_memory_u32(LIST_FRAME);
    if (frame != 0u) {
        *recomp_memory_u16(frame + 0xEu) = (uint16_t)cursor;
        *recomp_memory_u16(frame + 0xCu) = (uint16_t)cursor;
    }
    *recomp_memory_u16(LIST_CURSOR) = (uint16_t)cursor;
    *recomp_memory_u32(LIST_STATE_A) = 0u;
    *recomp_memory_u32(LIST_STATE_B) = 1u;
    *recomp_memory_u16(LIST_STATE_C) = 0u;
    *recomp_memory_u32(LIST_STATE_D) = 1u;
    *recomp_memory_u16(LIST_STATE_E) = 0xFu;
}

static void main_menu_setup(void)
{
    /* Back at the main menu without a match: the request is over. */
    if (session == VIEWER_WAITING) {
        requested = false;
        session = VIEWER_IDLE;
    }
    sub_000F3B30();
    add_item();
}

static bool confirm_pressed(void)
{
    for (uint32_t port_record = MENU_PORTS; port_record < MENU_PORTS_END; port_record += MENU_PORT_SIZE) {
        uint8_t port = *recomp_memory(port_record, 1u);
        if (*recomp_memory(PORT_ACTIVE + port, 1u) == 0u || *recomp_memory(port_record + 1u, 1u) == 0u) continue;
        if ((*recomp_memory_u32(PORT_PRESSED + port * PORT_PRESSED_SIZE) & PRESSED_CONFIRM) != 0u) return true;
    }
    return false;
}

static void main_menu_update(void)
{
    bool choose = *recomp_memory_u32(MENU_PHASE) == MENU_PHASE_RUNNING &&
        *recomp_memory_u32(MAIN_MENU_CURSOR) == ITEM_VIEWER && confirm_pressed();
    if (choose) *recomp_memory_u32(MAIN_MENU_CURSOR) = ITEM_EXHIBITION;
    sub_000F3E40();
    if (choose) {
        requested = true;
        session = VIEWER_WAITING;
        fprintf(stderr, "recomp viewer: chosen from the main menu\n");
    }
}

/* Once a frame: takes over the match the viewer asked for, and gives it
   back when the free camera is put away. */
/* The picture holds over what the viewer passes through unseen: Exhibition's
   first screen, which it confirms itself, and the match's opening. */
static bool holding(void)
{
    if (session != VIEWER_WAITING) return false;
    uint8_t scene = *recomp_memory(SCENE, 1u);
    if (scene == SCENE_MATCH) return match_frames < SETTLE_FRAMES;
    /* Only while the viewer confirms it (recomp_viewer_input): once the
       player moves the cursor off One Player, the screen is theirs. */
    return scene == SCENE_MENU && *recomp_memory(MENU_SCREEN, 1u) == SCREEN_EXHIBITION_SETUP &&
        *recomp_memory(SETUP_CURSOR, 1u) == 0u;
}

static void viewer_tick(uint32_t frame)
{
    recomp_suit_switch_frame(frame);
    recomp_d3d_frame_adapter_hold_present(RECOMP_PRESENT_HOLD_VIEWER, holding());
    /* One overlay a frame: a second publish could be drawn after the first. */
    RecompOverlayText text;
    bool panel = fill_title_hint(&text);
    if (choosing_court()) {
        fill_time_panel(&text);
        panel = true;
    }
    if (panel) recomp_overlay_publish(&text);
    else recomp_free_camera_overlay(session == VIEWER_VIEWING);
    if (session == VIEWER_IDLE) restore_courts();
    mask_character_select();
    bool in_match = *recomp_memory(SCENE, 1u) == SCENE_MATCH;
    switch (session) {
    case VIEWER_IDLE:
        break;
    case VIEWER_WAITING:
        if (!in_match) {
            match_frames = 0u;
            break;
        }
        /* The match's opening shows every player: the picture holds (see
           holding()) and the others are hidden until the viewer takes over. */
        recomp_actor_pose_set_hidden(OTHER_PLAYERS);
        if (++match_frames < SETTLE_FRAMES) break;
        recomp_actor_pose_choose(FIRST_PLAYER);
        recomp_actor_pose_set_hidden(OTHER_PLAYERS);
        recomp_free_camera_set_active(true);
        recomp_free_camera_frame_character();
        recomp_scene_freeze_set_overlay_hidden(true);
        session = VIEWER_VIEWING;
        fprintf(stderr, "recomp viewer: viewing\n");
        break;
    case VIEWER_VIEWING:
        if (recomp_free_camera_active() && in_match) {
            /* The match's score and portraits stay hidden, running or not,
               and so does the ball, which some poses leave in her hands. */
            recomp_scene_freeze_set_overlay_hidden(true);
            hide_ball(true);
            break;
        }
        hide_ball(false);
        recomp_free_camera_set_active(false);
        /* The match must run to be quit. */
        recomp_scene_freeze_set(false);
        recomp_scene_freeze_set_overlay_hidden(false);
        match_frames = 0u;
        if (in_match) {
            *recomp_memory_u32(RESULTS_CHOICE) = RESULTS_QUIT;
            *recomp_memory(MATCH_OVER, 1u) = 1u;
        }
        session = VIEWER_LEAVING;
        fprintf(stderr, "recomp viewer: leaving\n");
        break;
    case VIEWER_LEAVING: {
        uint8_t scene = *recomp_memory(SCENE, 1u);
        if (in_match) break;
        recomp_actor_pose_set_hidden(0u);
        recomp_scene_freeze_set_overlay_hidden(false);
        requested = false;
        if (scene == SCENE_TITLE) {
            if (*recomp_memory_u32(STEP_FRAMES) >= STEP_SETTLE_FRAMES &&
                *recomp_memory(STEP_LEAVING, 1u) == 0u) {
                *recomp_memory(STEP_LEAVING, 1u) = 1u;
            }
            break;
        }
        if (scene == SCENE_MENU || match_frames++ > 600u) session = VIEWER_IDLE;
        break;
    }
    }
}

void recomp_viewer_install(void)
{
    if (viewer_enabled()) recomp_scene_freeze_set_tick_hook(viewer_tick);
}

void recomp_viewer_input(RecompInputGamepad *gamepad)
{
    static uint32_t polls;
    choose_time(gamepad);
    uint8_t screen = *recomp_memory(MENU_SCREEN, 1u);
    bool picking = screen == SCREEN_CHARACTER_SELECT &&
        *recomp_memory(PICKS + PICKS_DONE, 1u) != 0u &&
        *recomp_memory(PICKS + PICK_STEP, 1u) != PICK_STEP_COURT;
    bool setup = screen == SCREEN_EXHIBITION_SETUP && *recomp_memory(SETUP_CURSOR, 1u) == 0u;
    if (gamepad == NULL || session != VIEWER_WAITING ||
        *recomp_memory(SCENE, 1u) != SCENE_MENU || (!picking && !setup)) {
        polls = 0u;
        return;
    }
    if (++polls % AUTO_PICK_POLLS == 0u) gamepad->analog_buttons[ANALOG_A] = 0xFFu;
}

/* While court select is up: [ ] or the triggers change the time of day;
   the triggers are kept from the game. */
static void choose_time(RecompInputGamepad *gamepad)
{
    if (gamepad == NULL || !choosing_court()) {
        time_back_held = time_next_held = false;
        return;
    }
    bool back = recomp_input_host_key(RECOMP_INPUT_HOST_KEY_LEFT_BRACKET) ||
        gamepad->analog_buttons[ANALOG_LTRIG] >= ANALOG_PRESSED;
    bool next = recomp_input_host_key(RECOMP_INPUT_HOST_KEY_RIGHT_BRACKET) ||
        gamepad->analog_buttons[ANALOG_RTRIG] >= ANALOG_PRESSED;
    int step = (next && !time_next_held) - (back && !time_back_held);
    time_back_held = back;
    time_next_held = next;
    gamepad->analog_buttons[ANALOG_LTRIG] = 0u;
    gamepad->analog_buttons[ANALOG_RTRIG] = 0u;
    if (step != 0) {
        chosen_time = recomp_viewer_time_step(chosen_time, step);
        fprintf(stderr, "recomp viewer: time of day %s\n", recomp_viewer_time_name(chosen_time));
    }
    apply_time();
}

RecompFunction recomp_viewer_menu_lookup_manual(uint32_t guest_address)
{
    if (!viewer_enabled()) return NULL;
    switch (guest_address) {
    case MAIN_MENU_SETUP: return main_menu_setup;
    case MAIN_MENU_UPDATE: return main_menu_update;
    default: return NULL;
    }
}
#else
RecompFunction recomp_viewer_menu_lookup_manual(uint32_t guest_address)
{
    (void)guest_address;
    return NULL;
}

bool recomp_viewer_menu_requested(void)
{
    return false;
}

void recomp_viewer_menu_clear(void)
{
}

void recomp_viewer_install(void)
{
}

void recomp_viewer_input(RecompInputGamepad *gamepad)
{
    (void)gamepad;
}
#endif
