#include "free_camera_adapter.h"
#include "free_camera.h"
#include "input_host_win32.h"
#include "scene_freeze_adapter.h"
#include "actor_pose_adapter.h"
#include "suit_switch_adapter.h"
#include "viewer_menu_adapter.h"
#include "overlay_text.h"
#include "screenshot.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef RECOMP_FULL_PROGRAM
#include "animation_probe.h"
#include "d3d_draw_adapter.h"
#include "d3d_frame_adapter.h"

/* Free camera: RECOMP_FREE_CAMERA=1 lets F1, or clicking the right stick,
   take the game's camera and fly it. It used to take BACK with the click,
   but the game saw that BACK first and backed out of its menus.

   The game keeps five camera objects of 0xAE0 bytes from 0x41A800. Each
   frame 0x00023B10 builds one from the object in EAX: it reads the eye at
   +0x180, the target at +0x190 and the vertical field of view in radians at
   +0x1B0, and writes the view and projection matrices at +0x50 and +0x90 that
   the frame's draws then use. The hook puts the free camera's eye, target
   and field of view in place for that build and leaves them there: the game
   culls each part of a character against the eye and target it finds in the
   object, so putting its own back made parts at the screen's edges vanish.
   When the free camera is put away the game's camera logic takes the object
   over again.

   Which object holds the main view depends on the scene: the volleyball
   demo builds only the first, the island map builds the first for the map
   and others for the portraits, and the hotel room builds the first for a
   view nothing shows and the third for the room. The free camera takes the
   object whose view the most draws used in the last frame (each draw's view
   transform is matched against each object's view matrix), again whenever
   the one it flies stops being built, and C or X moves it to the next
   object the game built in the last frame. Choosing by the order of builds
   and draws instead lost the view of the poolside's rest and hopping game.

   P or START freezes the scene while the camera flies (scene_freeze_adapter.c),
   H or the left shoulder hides the 2D overlay of a frozen scene, . or the
   right shoulder moves it on one frame, and putting the camera away thaws
   it. The game polls the controller from a task the freeze skips, so while
   frozen the freeze's frame hook samples it here once a frame, in any scene,
   so the scene can always be thawed.

   [ and ] or the d-pad's left and right play the previous or next animation
   clip on a character and hold it (actor_pose_adapter.c); \ or the left
   stick's click picks the next character and turns the camera to face her;
   - and = or BACK with the d-pad's down and up put her previous or next
   swimsuit on her (suit_switch_adapter.c). Changing the clip of a frozen
   scene steps it one frame so the new pose shows. Putting the camera away
   hands the animation back to the game. */
enum {
    BUILD_CAMERA = 0x00023B10u,
    CAMERA_FIRST = 0x0041A800u,
    CAMERA_STRIDE = 0x0AE0u,
    CAMERA_COUNT = 5u,
    CAMERA_VIEW = 0x50u,
    CAMERA_PROJECTION = 0x90u,
    SCENE_VIEW = 0x004D6F20u,
    SCENE_PROJECTION = 0x004D95F0u,
    CAMERA_EYE = 0x180u,
    CAMERA_TARGET = 0x190u,
    CAMERA_FOV = 0x1B0u,
    TRACE_LINES = 600u,
    XBOX_DPAD_UP = 0x0001u,
    XBOX_DPAD_DOWN = 0x0002u,
    XBOX_DPAD_LEFT = 0x0004u,
    XBOX_DPAD_RIGHT = 0x0008u,
    XBOX_START = 0x0010u,
    XBOX_LEFT_THUMB = 0x0040u,
    XBOX_BACK = 0x0020u,
    XBOX_RIGHT_THUMB = 0x0080u,
    ANALOG_A = 0u,
    ANALOG_B = 1u,
    ANALOG_X = 2u,
    ANALOG_Y = 3u,
    ANALOG_BLACK = 4u,
    ANALOG_WHITE = 5u,
    ANALOG_LTRIG = 6u,
    ANALOG_RTRIG = 7u,
    ANALOG_PRESSED = 0x20u,
};

static const float STICK_DEADZONE = 0.15f;
/* Holding a clip key: the first repeat after this long, then one every
   CLIP_REPEAT_MS, moving 3 clips at a time after CLIP_FASTER_AFTER_MS and 10
   after CLIP_FASTEST_AFTER_MS. */
enum {
    CLIP_REPEAT_DELAY_MS = 350,
    CLIP_REPEAT_MS = 60,
    CLIP_FASTER_AFTER_MS = 1500,
    CLIP_FASTEST_AFTER_MS = 3500,
};
/* At a field of view of one radian. */
static const float MOUSE_RADIANS_PER_PIXEL = 0.004f;
/* Framing a character: this far from her hips, raised a little, at this
   field of view. */
static const float FRAME_DISTANCE = 2.5f;
static const float FRAME_LIFT = 0.15f;
static const float FRAME_FOV = 0.75f;
/* The free camera steps once per presented frame. */
static const float FRAME_SECONDS = 1.0f / 60.0f;

/* A key or button held down to move through a list: a press moves one
   place, holding keeps moving, faster the longer it is held. */
typedef struct HeldRepeat {
    int direction;
    uint64_t since, next;
} HeldRepeat;

static struct {
    int enabled;
    RecompInputSampleSource source;
    bool active, started;
    bool toggle_held, reset_held, cycle_held, freeze_held, overlay_held;
    bool actor_held, shot_held;
    HeldRepeat clip_held;
    HeldRepeat suit_held, step_repeat;
    /* Face the chosen character at the next camera build; frame_pending
       also moves close to her. */
    bool aim_pending, frame_pending;
    /* The model viewer has the camera (recomp_free_camera_overlay). */
    bool viewer;
    /* The view the model viewer first framed her in, which reset returns to. */
    RecompFreeCamera home;
    bool home_saved, save_home;
    /* H's clean view also hides the runtime's own overlay. */
    bool panel_hidden;
    /* The hint lists controller buttons once a controller was last used,
       and from the start on a Steam Deck; keys once a key was. */
    int controller_hints;
    uint32_t slot;
    /* Objects built in the frame being drawn and in the one before it, and
       the frame the game last built one itself. */
    uint32_t built_frame, built, built_before, game_built_frame;
    /* Draws that used each object's view, in this frame and the last. */
    uint32_t drawn[CAMERA_COUNT], drawn_before[CAMERA_COUNT];
    /* The fallback build of a frozen frame (frame_started) is under way. */
    bool fallback;
    /* C or X picked the object flown. */
    bool picked;
    uint32_t stepped_frame;
    RecompInputGamepad pad;
    RecompFreeCamera camera;
    uint32_t trace_lines;
} state = {.enabled = -1, .controller_hints = -1};

static uint64_t milliseconds(void)
{
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) == 0) return 0u;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* How many places to move now (signed), given the direction held (-1, 0 or
   1). *released is set when the keys were just let go. */
static int held_repeat(HeldRepeat *held, int direction, uint64_t now, bool *released)
{
    *released = false;
    if (direction != 0 && direction != held->direction) {
        held->direction = direction;
        held->since = now;
        held->next = now + CLIP_REPEAT_DELAY_MS;
        return direction;
    }
    if (direction != 0 && now >= held->next) {
        uint64_t time = now - held->since;
        int places = time >= CLIP_FASTEST_AFTER_MS ? 10 : time >= CLIP_FASTER_AFTER_MS ? 3 : 1;
        held->next = now + CLIP_REPEAT_MS;
        return direction * places;
    }
    if (direction == 0 && held->direction != 0) {
        held->direction = 0;
        *released = true;
    }
    return 0;
}

static const char *setting(const char *name)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : NULL;
}

static bool enabled(void)
{
    if (state.enabled < 0) {
        const char *value = setting("RECOMP_FREE_CAMERA");
        state.enabled = value != NULL && strcmp(value, "0") != 0;
        const char *slot = setting("RECOMP_FREE_CAMERA_SLOT");
        state.slot = slot != NULL ? (uint32_t)strtoul(slot, NULL, 10) : 0u;
        if (state.slot >= CAMERA_COUNT) state.slot = 0u;
    }
    return state.enabled != 0;
}

/* RECOMP_FREE_CAMERA_TRACE=<frame> logs camera builds from that frame on;
   =on logs them each time the free camera is turned on. */
static bool tracing(uint32_t frame)
{
    const char *from = setting("RECOMP_FREE_CAMERA_TRACE");
    if (from == NULL || state.trace_lines >= TRACE_LINES) return false;
    if (strcmp(from, "on") == 0) return state.active;
    return frame >= (uint32_t)strtoul(from, NULL, 10);
}

/* True when texture replacement keeps plain F12 for itself (screenshot.h).
   Only the Windows presenter has it. */
static bool texture_keys(void)
{
#ifdef _WIN32
    static int on = -1;
    if (on < 0) on = recomp_texture_keys_enabled(getenv("RECOMP_TEXTURE_DUMP"), getenv("RECOMP_TEXTURES"));
    return on != 0;
#else
    return false;
#endif
}

static void set_active(bool active)
{
    if (state.active == active) return;
    state.active = active;
    state.started = false;
    state.trace_lines = 0u;
    if (!active) {
        state.home_saved = false;
        state.picked = false;
        /* Putting the camera away hands the scene back, running. */
        recomp_actor_pose_release();
        recomp_suit_switch_forget();
        recomp_scene_freeze_set(false);
        recomp_scene_freeze_set_overlay_hidden(false);
        state.panel_hidden = false;
    }
    fprintf(stderr, "recomp free camera: %s\n", active ? "on" : "off");
}

static void sample_while_frozen(uint32_t frame);
static void frame_started(void);
static void draw_viewed(const float view[16]);

void recomp_free_camera_set_source(RecompInputSampleSource source)
{
    state.source = source;
    /* Off, the free camera leaves the game alone: only the input passes
       through it. */
    if (!enabled()) return;
    recomp_scene_freeze_set_frame_hook(sample_while_frozen);
    recomp_d3d_frame_adapter_set_first_clear_hook(frame_started);
    recomp_d3d_draw_set_view_observer(draw_viewed);
}

/* Handles the toggles in one sample of port 0. True when the sample is the
   free camera's rather than the game's. */
static float stick(int16_t value);

static bool controller_used(const RecompInputGamepad *gamepad)
{
    if (gamepad->buttons != 0u) return true;
    for (unsigned i = 0u; i < sizeof gamepad->analog_buttons; ++i) {
        if (gamepad->analog_buttons[i] >= ANALOG_PRESSED) return true;
    }
    return stick(gamepad->thumb_lx) != 0.0f || stick(gamepad->thumb_ly) != 0.0f ||
        stick(gamepad->thumb_rx) != 0.0f || stick(gamepad->thumb_ry) != 0.0f;
}

static bool keys_used(void)
{
    for (int key = 0; key < RECOMP_INPUT_HOST_KEY_COUNT; ++key) {
        if (recomp_input_host_key((RecompInputHostKey)key)) return true;
    }
    return false;
}

/* The input host also presses controller buttons for some keys, for the
   game (input_host_win32.c). The free camera reads those keys itself, so
   their buttons are left out of what it reads: holding S would otherwise
   press Y and reset the view, A would press X and change the camera object,
   W and Q would toggle the clean view and step, and the arrows would press
   the d-pad and change the animation and swimsuit. */
static void leave_out_keys(RecompInputGamepad *pad)
{
    static const struct {
        RecompInputHostKey key;
        unsigned analog;
    } analog_keys[] = {
        {RECOMP_INPUT_HOST_KEY_A, ANALOG_X}, {RECOMP_INPUT_HOST_KEY_S, ANALOG_Y},
        {RECOMP_INPUT_HOST_KEY_Z, ANALOG_A}, {RECOMP_INPUT_HOST_KEY_X, ANALOG_B},
        {RECOMP_INPUT_HOST_KEY_Q, ANALOG_WHITE}, {RECOMP_INPUT_HOST_KEY_W, ANALOG_BLACK},
        {RECOMP_INPUT_HOST_KEY_E, ANALOG_LTRIG}, {RECOMP_INPUT_HOST_KEY_R, ANALOG_RTRIG},
    };
    static const struct {
        RecompInputHostKey key;
        uint16_t button;
    } button_keys[] = {
        {RECOMP_INPUT_HOST_KEY_UP, XBOX_DPAD_UP}, {RECOMP_INPUT_HOST_KEY_DOWN, XBOX_DPAD_DOWN},
        {RECOMP_INPUT_HOST_KEY_LEFT, XBOX_DPAD_LEFT}, {RECOMP_INPUT_HOST_KEY_RIGHT, XBOX_DPAD_RIGHT},
    };
    for (size_t i = 0u; i < sizeof analog_keys / sizeof analog_keys[0]; ++i) {
        if (recomp_input_host_key(analog_keys[i].key)) pad->analog_buttons[analog_keys[i].analog] = 0u;
    }
    for (size_t i = 0u; i < sizeof button_keys / sizeof button_keys[0]; ++i) {
        if (recomp_input_host_key(button_keys[i].key)) pad->buttons &= (uint16_t)~button_keys[i].button;
    }
}

static bool take_sample(const RecompInputGamepad *raw)
{
    RecompInputGamepad pad = *raw;
    leave_out_keys(&pad);
    const RecompInputGamepad *gamepad = &pad;
    if (state.controller_hints < 0) {
        const char *deck = getenv("SteamDeck");
        state.controller_hints = deck != NULL && strcmp(deck, "1") == 0;
    }
    if (keys_used()) state.controller_hints = 0;
    else if (controller_used(gamepad)) state.controller_hints = 1;

    bool toggle = recomp_input_host_key(RECOMP_INPUT_HOST_KEY_F1) ||
        (gamepad->buttons & XBOX_RIGHT_THUMB) != 0u;
    if (toggle && !state.toggle_held) set_active(!state.active);
    state.toggle_held = toggle;

    /* F12, or Shift+F12 beside texture replacement, saves a screenshot
       anywhere; Back with A while flying. */
    bool shot = recomp_screenshot_key(recomp_input_host_key(RECOMP_INPUT_HOST_KEY_F12),
                    recomp_input_host_key(RECOMP_INPUT_HOST_KEY_SHIFT), texture_keys()) ||
        (state.active && (gamepad->buttons & XBOX_BACK) != 0u &&
         gamepad->analog_buttons[ANALOG_A] >= ANALOG_PRESSED);
    if (shot && !state.shot_held) recomp_screenshot_request();
    state.shot_held = shot;

    /* P pauses anywhere, flying or not; Start only while flying, since the
       game has its own use for it. The game's keyboard bindings leave P, H
       and . free. */
    bool freeze = recomp_input_host_key(RECOMP_INPUT_HOST_KEY_P) ||
        (state.active && (gamepad->buttons & XBOX_START) != 0u);
    if (freeze && !state.freeze_held) {
        bool pause = !recomp_scene_freeze_active();
        recomp_scene_freeze_set(pause);
        /* A trace starts again at each pause. */
        state.trace_lines = 0u;
        if (!pause && !state.active) {
            recomp_scene_freeze_set_overlay_hidden(false);
            state.panel_hidden = false;
        }
    }
    state.freeze_held = freeze;
    const bool paused = recomp_scene_freeze_active();

    bool overlay = (state.active || paused) && (recomp_input_host_key(RECOMP_INPUT_HOST_KEY_H) ||
        (state.active && gamepad->analog_buttons[ANALOG_BLACK] >= ANALOG_PRESSED));
    if (overlay && !state.overlay_held) {
        /* A clean view hides the game's overlay and the runtime's panel
           together; the next press shows both. The model viewer keeps the
           game's overlay hidden throughout, so there it shows the panel. */
        bool hide = !state.panel_hidden;
        state.panel_hidden = hide;
        recomp_scene_freeze_set_overlay_hidden(hide);
    }
    state.overlay_held = overlay;

    bool step = (state.active || paused) && (recomp_input_host_key(RECOMP_INPUT_HOST_KEY_PERIOD) ||
        (state.active && gamepad->analog_buttons[ANALOG_WHITE] >= ANALOG_PRESSED));
    /* Holding it steps on and on, every frame after a while. */
    bool step_released;
    int steps = held_repeat(&state.step_repeat, step ? 1 : 0, milliseconds(), &step_released);
    if (steps != 0 || (step && milliseconds() - state.step_repeat.since >= CLIP_FASTER_AFTER_MS)) {
        recomp_scene_freeze_step();
    }

    /* A press moves to the next or previous clip; holding keeps moving,
       faster the longer it is held, and the clip plays when let go. */
    bool clip_back = state.active && (recomp_input_host_key(RECOMP_INPUT_HOST_KEY_LEFT_BRACKET) ||
        (gamepad->buttons & XBOX_DPAD_LEFT) != 0u);
    bool clip_next = state.active && (recomp_input_host_key(RECOMP_INPUT_HOST_KEY_RIGHT_BRACKET) ||
        (gamepad->buttons & XBOX_DPAD_RIGHT) != 0u);
    uint64_t now = milliseconds();
    bool released;
    int places = held_repeat(&state.clip_held, (int)clip_next - (int)clip_back, now, &released);
    if (places != 0) recomp_actor_pose_browse(places);
    if (released && recomp_actor_pose_play_browsed() && recomp_scene_freeze_active()) {
        recomp_scene_freeze_step();
    }

    /* The model viewer shows one girl, so it has no next one. */
    bool actor = state.active && !state.viewer && (recomp_input_host_key(RECOMP_INPUT_HOST_KEY_BACKSLASH) ||
        (gamepad->buttons & XBOX_LEFT_THUMB) != 0u);
    if (actor && !state.actor_held) {
        recomp_actor_pose_next_actor();
        state.aim_pending = true;
    }
    state.actor_held = actor;

    /* Swimsuits move the same way and the suit loads when let go. */
    bool suit_back = state.active && (recomp_input_host_key(RECOMP_INPUT_HOST_KEY_MINUS) ||
        (gamepad->buttons & XBOX_DPAD_DOWN) != 0u);
    bool suit_next = state.active && (recomp_input_host_key(RECOMP_INPUT_HOST_KEY_EQUALS) ||
        (gamepad->buttons & XBOX_DPAD_UP) != 0u);
    places = held_repeat(&state.suit_held, (int)suit_next - (int)suit_back, now, &released);
    if (places != 0) {
        RecompActorPoseStatus pose;
        recomp_actor_pose_status(&pose);
        recomp_suit_switch_browse(pose.actor, places);
    }
    if (released) recomp_suit_switch_wear_browsed();

    if (state.active || toggle) state.pad = *gamepad;
    return state.active || toggle;
}

bool recomp_free_camera_sample(uint32_t port, RecompInputGamepad *gamepad)
{
    bool present = false;

    if (gamepad == NULL) return false;
    memset(gamepad, 0, sizeof *gamepad);
    if (state.source != NULL) present = state.source(port, gamepad);
    if (port != 0u || !enabled()) return present;
    if (take_sample(gamepad)) memset(gamepad, 0, sizeof *gamepad);
    else recomp_viewer_input(gamepad);
    return present;
}

/* While frozen the game stops polling; the freeze calls this once a frame. */
static void sample_while_frozen(uint32_t frame)
{
    (void)frame;
    RecompInputGamepad gamepad;
    memset(&gamepad, 0, sizeof gamepad);
    if (state.source != NULL) state.source(0u, &gamepad);
    take_sample(&gamepad);
}

void sub_00023B10(void);

static float stick(int16_t value)
{
    float axis = (float)value / 32767.0f;
    if (axis > 1.0f) axis = 1.0f;
    if (axis < -1.0f) axis = -1.0f;
    if (axis > -STICK_DEADZONE && axis < STICK_DEADZONE) return 0.0f;
    return axis;
}

static float key(RecompInputHostKey positive, RecompInputHostKey negative)
{
    return (recomp_input_host_key(positive) ? 1.0f : 0.0f) -
        (recomp_input_host_key(negative) ? 1.0f : 0.0f);
}

static bool analog(const RecompInputGamepad *pad, unsigned index)
{
    return pad->analog_buttons[index] >= ANALOG_PRESSED;
}

static void read_controls(RecompFreeCameraControls *controls)
{
    const RecompInputGamepad *pad = &state.pad;

    memset(controls, 0, sizeof *controls);
    controls->move[0] = stick(pad->thumb_lx) + key(RECOMP_INPUT_HOST_KEY_D, RECOMP_INPUT_HOST_KEY_A);
    controls->move[1] = ((float)pad->analog_buttons[ANALOG_RTRIG] -
        (float)pad->analog_buttons[ANALOG_LTRIG]) / 255.0f +
        key(RECOMP_INPUT_HOST_KEY_E, RECOMP_INPUT_HOST_KEY_Q);
    controls->move[2] = stick(pad->thumb_ly) + key(RECOMP_INPUT_HOST_KEY_W, RECOMP_INPUT_HOST_KEY_S);
    controls->look[0] = stick(pad->thumb_rx) +
        key(RECOMP_INPUT_HOST_KEY_RIGHT, RECOMP_INPUT_HOST_KEY_LEFT);
    controls->look[1] = stick(pad->thumb_ry) +
        key(RECOMP_INPUT_HOST_KEY_UP, RECOMP_INPUT_HOST_KEY_DOWN);
    /* The d-pad's up and down change the swimsuit; only keys zoom. */
    controls->zoom = key(RECOMP_INPUT_HOST_KEY_Z, RECOMP_INPUT_HOST_KEY_X);
    /* Dragging with the right mouse button aims; a narrow view aims finer. */
    float drag_x, drag_y;
    if (recomp_input_host_mouse_drag(&drag_x, &drag_y)) {
        controls->turn[0] = drag_x * MOUSE_RADIANS_PER_PIXEL * state.camera.fov;
        controls->turn[1] = -drag_y * MOUSE_RADIANS_PER_PIXEL * state.camera.fov;
    }
    controls->fast = analog(pad, ANALOG_A) || recomp_input_host_key(RECOMP_INPUT_HOST_KEY_SHIFT);
    controls->slow = analog(pad, ANALOG_B) || recomp_input_host_key(RECOMP_INPUT_HOST_KEY_CONTROL);

    /* Debug: RECOMP_FREE_CAMERA_DRIFT=right,up,forward,turn,pitch adds a
       constant input, so a scripted run can move the camera. */
    const char *drift = setting("RECOMP_FREE_CAMERA_DRIFT");
    float add[5] = {0};
    if (drift != NULL && sscanf(drift, "%f,%f,%f,%f,%f",
        &add[0], &add[1], &add[2], &add[3], &add[4]) == 5) {
        for (unsigned i = 0u; i < 3u; ++i) controls->move[i] += add[i];
        controls->look[0] += add[3];
        controls->look[1] += add[4];
    }
}

/* Reset also hands the chosen character's animation back to the game and
   puts her first swimsuit back on. */
static void reset_character(void)
{
    RecompActorPoseStatus pose;
    recomp_actor_pose_status(&pose);
    bool posed = pose.holding;
    recomp_actor_pose_release();
    recomp_suit_switch_restore(pose.actor);
    if (posed && recomp_scene_freeze_active()) recomp_scene_freeze_step();
}

/* Counts a draw against the objects whose view it used. */
static void draw_viewed(const float view[16])
{
    for (uint32_t slot = 0u; slot < CAMERA_COUNT; ++slot) {
        float matrix[16];
        recomp_guest_load(matrix, CAMERA_FIRST + slot * CAMERA_STRIDE + CAMERA_VIEW, sizeof matrix);
        bool same = true;
        for (unsigned i = 0u; i < 16u && same; ++i) {
            float difference = matrix[i] - view[i];
            float size = fabsf(matrix[i]) > 1.0f ? fabsf(matrix[i]) : 1.0f;
            same = fabsf(difference) <= 1e-4f * size;
        }
        if (same) ++state.drawn[slot];
    }
}

/* Flies the object the scene is drawn with: on the way in, and whenever
   the one flown stops being built. */
static void choose_object(void)
{
    if (state.built_before == 0u) return;
    if ((state.started || state.picked) && (state.built_before & (1u << state.slot)) != 0u) return;
    state.picked = false;
    uint32_t best = state.slot;
    for (uint32_t slot = 0u; slot < CAMERA_COUNT; ++slot) {
        if ((state.built_before & (1u << slot)) == 0u) continue;
        if ((state.built_before & (1u << best)) == 0u ||
            state.drawn_before[slot] > state.drawn_before[best]) best = slot;
    }
    state.slot = best;
    state.started = false;
    fprintf(stderr, "recomp free camera: object %u (built %02x, draws %u %u %u %u %u)\n", best,
        state.built_before, state.drawn_before[0], state.drawn_before[1], state.drawn_before[2],
        state.drawn_before[3], state.drawn_before[4]);
}

static void build_camera(void)
{
    const uint32_t object = recomp_runtime.registers.eax;
    const uint32_t frame = recomp_d3d_frame_adapter_swap_counter();
    const uint32_t slot = (object - CAMERA_FIRST) / CAMERA_STRIDE;
    const bool known = object >= CAMERA_FIRST && slot < CAMERA_COUNT &&
        object == CAMERA_FIRST + slot * CAMERA_STRIDE;

    if (!state.fallback) state.game_built_frame = frame;
    if (state.built_frame != frame) {
        recomp_actor_pose_frame();
        state.built_before = state.built;
        state.built = 0u;
        memcpy(state.drawn_before, state.drawn, sizeof state.drawn);
        memset(state.drawn, 0, sizeof state.drawn);
        state.built_frame = frame;
        if (state.active) choose_object();
    }
    if (known) state.built |= 1u << slot;
    recomp_suit_switch_camera_build();
    if (state.active) {
        bool cycle = analog(&state.pad, ANALOG_X) || recomp_input_host_key(RECOMP_INPUT_HOST_KEY_C);
        if (cycle && !state.cycle_held) {
            for (uint32_t step = 1u; step <= CAMERA_COUNT; ++step) {
                uint32_t next = (state.slot + step) % CAMERA_COUNT;
                if ((state.built_before & (1u << next)) != 0u) {
                    if (next != state.slot) {
                        state.slot = next;
                        state.picked = true;
                        state.started = false;
                        fprintf(stderr, "recomp free camera: object %u\n", next);
                    }
                    break;
                }
            }
        }
        state.cycle_held = cycle;
    }
    const bool ours = known && slot == state.slot;
    float eye[3], target[3], fov;

    recomp_guest_load(eye, object + CAMERA_EYE, sizeof eye);
    recomp_guest_load(target, object + CAMERA_TARGET, sizeof target);
    recomp_guest_load(&fov, object + CAMERA_FOV, sizeof fov);
    if (tracing(frame)) {
        ++state.trace_lines;
        /* The game task building it: the scheduler's current record's function. */
        uint32_t task = *recomp_memory_u32(0x005F2E5Cu);
        uint32_t function = task < 17u ? *recomp_memory_u32(0x005F2C38u + task * 0x20u + 0x0Cu) : 0u;
        fprintf(stderr, "recomp free camera trace: frame=%u object=%08x active=%d task=%08x "
            "eye=%.3f,%.3f,%.3f target=%.3f,%.3f,%.3f fov=%.4f\n",
            frame, object, state.active && ours, function, eye[0], eye[1], eye[2],
            target[0], target[1], target[2], fov);
    }

    const bool fly = state.active && ours;
    if (fly) {
        bool reset = analog(&state.pad, ANALOG_Y) || recomp_input_host_key(RECOMP_INPUT_HOST_KEY_R);
        bool resetting = reset && !state.reset_held;
        if (resetting) reset_character();
        if (resetting && state.viewer && state.started && state.home_saved) {
            /* The viewer's view is the one it first framed her in; framing
               her again would follow her moving hips. */
            state.camera = state.home;
        } else if ((!state.started || resetting) &&
            recomp_free_camera_begin(&state.camera, eye, target, fov)) {
            state.started = true;
            state.stepped_frame = frame;
        }
        state.reset_held = reset;
        if (state.started && state.stepped_frame != frame) {
            RecompFreeCameraControls controls;
            read_controls(&controls);
            recomp_free_camera_step(&state.camera, &controls, FRAME_SECONDS);
            state.stepped_frame = frame;
        }
    }
    if (fly && state.started && (state.aim_pending || state.frame_pending)) {
        float point[3];
        if (recomp_actor_pose_focus(point)) {
            if (state.frame_pending) {
                float facing[3];
                point[1] += FRAME_LIFT;
                if (recomp_actor_pose_facing(facing)) {
                    /* Stand in front of her: frame from a point ahead of
                       her hips, so the line of sight runs back to them. */
                    for (unsigned i = 0u; i < 3u; ++i) {
                        state.camera.position[i] = point[i] + facing[i] * FRAME_DISTANCE;
                    }
                }
                recomp_free_camera_frame(&state.camera, point, FRAME_DISTANCE, FRAME_FOV);
            } else {
                recomp_free_camera_look_at(&state.camera, point);
            }
        }
        if (state.frame_pending && state.save_home) {
            state.home = state.camera;
            state.home_saved = true;
            state.save_home = false;
        }
        state.aim_pending = false;
        state.frame_pending = false;
    }
    if (fly && state.started) {
        float flown[3];
        recomp_free_camera_target(&state.camera, flown);
        recomp_guest_store(object + CAMERA_EYE, state.camera.position, sizeof state.camera.position);
        recomp_guest_store(object + CAMERA_TARGET, flown, sizeof flown);
        recomp_guest_store(object + CAMERA_FOV, &state.camera.fov, sizeof state.camera.fov);
    }

    /* The animation probe observes the same build when its traces run. */
    RecompFunction observed = recomp_animation_probe_lookup_manual(BUILD_CAMERA);
    if (observed != NULL) observed();
    else sub_00023B10();

}

static bool same_matrix(const float a[16], const float b[16])
{
    for (unsigned i = 0u; i < 16u; ++i) {
        float size = fabsf(a[i]) > 1.0f ? fabsf(a[i]) : 1.0f;
        if (fabsf(a[i] - b[i]) > 1e-4f * size) return false;
    }
    return true;
}

/* Puts `matrix` at `address` when it holds `old`. */
static bool replace_matrix(uint32_t address, const float old[16], const float matrix[16])
{
    float current[16];
    recomp_guest_load(current, address, sizeof current);
    if (!same_matrix(current, old)) return false;
    recomp_guest_store(address, matrix, sizeof current);
    return true;
}

/* Some scenes build the camera in a task that does not draw (the pool's
   hopping game), which a frozen scene skips, so the view would stop with
   it. At the start of each frozen frame, if no camera was built in the last
   one, the free camera builds its object itself. That task also copied the
   view and projection to the globals the drawing task hands the device
   (0x004D6F20 and 0x004D95F0, copied by 0x001221E0), so where they, and the
   device, still hold the object's last matrices the new ones replace them. */
static void frame_started(void)
{
    if (!state.active || !state.started || !recomp_scene_freeze_holding()) return;
    uint32_t frame = recomp_d3d_frame_adapter_swap_counter();
    if (state.game_built_frame + 1u >= frame) return;
    const RecompRegisters registers = recomp_runtime.registers;
    RecompFpuContext fpu;
    recomp_fpu_context_save(&fpu);
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = 0u;
    const uint32_t object = CAMERA_FIRST + state.slot * CAMERA_STRIDE;
    float old_view[16], old_projection[16], view[16], projection[16];
    recomp_guest_load(old_view, object + CAMERA_VIEW, sizeof old_view);
    recomp_guest_load(old_projection, object + CAMERA_PROJECTION, sizeof old_projection);
    recomp_runtime.registers.eax = object;
    state.fallback = true;
    build_camera();
    state.fallback = false;
    recomp_guest_load(view, object + CAMERA_VIEW, sizeof view);
    recomp_guest_load(projection, object + CAMERA_PROJECTION, sizeof projection);
    bool replaced = recomp_d3d_draw_replace_view(old_view, view, projection);
    bool copied = replace_matrix(SCENE_VIEW, old_view, view);
    copied = replace_matrix(SCENE_PROJECTION, old_projection, projection) && copied;
    if (tracing(frame)) {
        ++state.trace_lines;
        fprintf(stderr, "recomp free camera trace: frame=%u frozen, so built here; device view %s; "
            "scene copies %s; last frame's draws with each object's view %u %u %u %u %u\n",
            frame, replaced ? "replaced" : "left (not the object's)",
            copied ? "replaced" : "not both the object's", state.drawn_before[0],
            state.drawn_before[1], state.drawn_before[2], state.drawn_before[3], state.drawn_before[4]);
    }
    recomp_runtime.registers = registers;
    recomp_fpu_context_restore(&fpu);
}

RecompFunction recomp_free_camera_lookup_manual(uint32_t guest_address)
{
    if (!enabled()) return NULL;
    if (guest_address == BUILD_CAMERA) return build_camera;
    return recomp_actor_pose_lookup_manual(guest_address);
}
void recomp_free_camera_set_active(bool active)
{
    if (enabled()) set_active(active);
}

bool recomp_free_camera_enabled(void)
{
    return enabled();
}

bool recomp_free_camera_active(void)
{
    return state.active;
}

void recomp_free_camera_face_character(void)
{
    state.aim_pending = true;
}

void recomp_free_camera_frame_character(void)
{
    state.frame_pending = true;
    state.save_home = true;
}

static void fill_overlay(RecompOverlayText *text, bool viewer)
{
    const char *shot = texture_keys() ? "Shift+F12" : "F12";
    char hint[RECOMP_OVERLAY_HINT_SIZE];
    recomp_overlay_clear(text);
    if (state.panel_hidden) return;
    if (!state.active) {
        if (!recomp_scene_freeze_active()) return;
        recomp_overlay_set_title(text, "PAUSED");
        snprintf(hint, sizeof hint,
            "P resume  |  . step  |  H clean view  |  %s screenshot  |  F1 free camera", shot);
        recomp_overlay_set_hint(text, state.controller_hints > 0 ? "P resume  |  R3 free camera" : hint);
        return;
    }

    RecompActorPoseStatus pose;
    char name[48];
    recomp_actor_pose_status(&pose);
    recomp_overlay_set_title(text, viewer ? "MODEL VIEWER" : "FREE CAMERA");
    if (pose.active) {
        recomp_actor_pose_name(pose.actor, name, sizeof name);
        recomp_overlay_add_line(text, "%s", name);
        if (pose.browsing) {
            recomp_overlay_add_line(text, "Animation %u of %u (clip %04X)",
                (unsigned)pose.browse_position, (unsigned)pose.browse_count, (unsigned)pose.browse_clip);
        } else if (pose.holding) {
            recomp_overlay_add_line(text, "Animation %u of %u (clip %04X)",
                (unsigned)pose.position, (unsigned)pose.count, (unsigned)pose.clip);
        } else {
            recomp_overlay_add_line(text, "Animation: the game's own");
        }
        uint16_t suit = recomp_suit_switch_shown(pose.actor);
        uint32_t place = 0u, count = 0u;
        if (suit != 0u) {
            char suit_name[48];
            recomp_suit_switch_name(suit, suit_name, sizeof suit_name);
            recomp_suit_switch_place(pose.actor, suit, &place, &count);
            recomp_overlay_add_line(text, "Swimsuit %u of %u: %s%s", (unsigned)place,
                (unsigned)count, suit_name, recomp_suit_switch_busy() ? " (changing)" : "");
        }
    }
    recomp_overlay_add_line(text, "%s", recomp_scene_freeze_active() ? "Frozen" : "Running");
    if (state.controller_hints > 0) {
        recomp_overlay_set_hint(text, viewer
            ? "R3 leave  |  Start freeze  |  RB step  |  LB clean view  |  Back + A screenshot  |  Y reset\n"
              "Left stick move  |  LT RT down/up  |  Right stick look\n"
              "D-pad left/right animation  |  D-pad up/down swimsuit"
            : "R3 off  |  Start freeze  |  RB step  |  LB clean view  |  Back + A screenshot  |  Y reset\n"
              "Left stick move  |  LT RT down/up  |  Right stick look  |  L3 next girl\n"
              "D-pad left/right animation  |  D-pad up/down swimsuit");
    } else {
        snprintf(hint, sizeof hint, viewer
            ? "F1 leave  |  P freeze  |  . step  |  H clean view  |  %s screenshot  |  R reset\n"
              "WASD QE move  |  arrows / right-drag look  |  Z X zoom\n"
              "[ ] animation  |  - = swimsuit"
            : "F1 off  |  P freeze  |  . step  |  H clean view  |  %s screenshot  |  R reset\n"
              "WASD QE move  |  arrows / right-drag look  |  Z X zoom  |  \\ next girl\n"
              "[ ] animation  |  - = swimsuit", shot);
        recomp_overlay_set_hint(text, hint);
    }
}

void recomp_free_camera_overlay(bool viewer)
{
    RecompOverlayText text;
    state.viewer = viewer;
    fill_overlay(&text, viewer);
    recomp_overlay_publish(&text);
}
#else
RecompFunction recomp_free_camera_lookup_manual(uint32_t guest_address)
{
    (void)guest_address;
    return NULL;
}

void recomp_free_camera_set_active(bool active)
{
    (void)active;
}

bool recomp_free_camera_enabled(void)
{
    return false;
}

bool recomp_free_camera_active(void)
{
    return false;
}

void recomp_free_camera_face_character(void)
{
}

void recomp_free_camera_frame_character(void)
{
}

void recomp_free_camera_overlay(bool viewer)
{
    (void)viewer;
}
#endif
