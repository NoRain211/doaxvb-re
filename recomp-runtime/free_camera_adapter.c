#include "free_camera_adapter.h"
#include "free_camera.h"
#include "input_host_win32.h"
#include "scene_freeze_adapter.h"
#include "actor_pose_adapter.h"
#include "viewer_menu_adapter.h"
#include "overlay_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RECOMP_FULL_PROGRAM
#include "animation_probe.h"
#include "d3d_frame_adapter.h"

/* Free camera: RECOMP_FREE_CAMERA=1 lets F1, or BACK with the right stick
   clicked, take the game's camera and fly it.

   The game keeps five camera objects of 0xAE0 bytes from 0x41A800. Each
   frame 0x00023B10 builds one from the object in EAX: it reads the eye at
   +0x180, the target at +0x190 and the vertical field of view in radians at
   +0x1B0, and writes the view and projection matrices at +0x50 and +0x90 that
   the frame's draws and culling then use. The hook puts the free camera's
   eye, target and field of view in place for that build and puts the game's
   own back afterwards, so the game's camera logic carries on undisturbed and
   the view resumes from it when the free camera is put away.

   Which object holds the main view depends on the scene: the volleyball
   demo builds only the first, while the island map builds the first for the
   map and others for the portraits. The free camera starts on the first, and
   C or X moves it to the next object the game built in the last frame.

   P or START freezes the scene while the camera flies (scene_freeze_adapter.c),
   H or the left shoulder hides the 2D overlay of a frozen scene, . or the
   right shoulder moves it on one frame, and putting the camera away thaws
   it. The game polls the controller from a task the freeze skips, so while
   frozen the freeze's frame hook samples it here once a frame, in any scene,
   so the scene can always be thawed.

   [ and ] or the d-pad's left and right play the previous or next animation
   clip on a character and hold it (actor_pose_adapter.c); \ or the left
   stick's click picks the next character and turns the camera to face her. Changing the clip of a frozen scene
   steps it one frame so the new pose shows. Putting the camera away hands
   the animation back to the game. */
enum {
    BUILD_CAMERA = 0x00023B10u,
    CAMERA_FIRST = 0x0041A800u,
    CAMERA_STRIDE = 0x0AE0u,
    CAMERA_COUNT = 5u,
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
/* At a field of view of one radian. */
static const float MOUSE_RADIANS_PER_PIXEL = 0.004f;
/* Framing a character: this far from her hips, raised a little, at this
   field of view. */
static const float FRAME_DISTANCE = 2.5f;
static const float FRAME_LIFT = 0.15f;
static const float FRAME_FOV = 0.75f;
/* The free camera steps once per presented frame. */
static const float FRAME_SECONDS = 1.0f / 60.0f;

static struct {
    int enabled;
    RecompInputSampleSource source;
    bool active, started;
    bool toggle_held, reset_held, cycle_held, freeze_held, overlay_held, step_held;
    bool clip_back_held, clip_next_held, actor_held;
    /* Face the chosen character at the next camera build; frame_pending
       also moves close to her. */
    bool aim_pending, frame_pending;
    /* H's clean view also hides the runtime's own overlay. */
    bool panel_hidden;
    uint32_t slot;
    /* Objects built in the frame being drawn and in the one before it. */
    uint32_t built_frame, built, built_before;
    uint32_t stepped_frame;
    RecompInputGamepad pad;
    RecompFreeCamera camera;
    uint32_t trace_lines;
} state = {.enabled = -1};

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

/* RECOMP_FREE_CAMERA_TRACE=<frame> logs camera builds from that frame on. */
static bool tracing(uint32_t frame)
{
    const char *from = setting("RECOMP_FREE_CAMERA_TRACE");
    return from != NULL && frame >= (uint32_t)strtoul(from, NULL, 10) &&
        state.trace_lines < TRACE_LINES;
}

static void set_active(bool active)
{
    if (state.active == active) return;
    state.active = active;
    state.started = false;
    if (!active) {
        recomp_scene_freeze_set(false);
        recomp_scene_freeze_set_overlay_hidden(false);
        recomp_actor_pose_release();
        state.panel_hidden = false;
    }
    fprintf(stderr, "recomp free camera: %s\n", active ? "on" : "off");
}

static void sample_while_frozen(uint32_t frame);

void recomp_free_camera_set_source(RecompInputSampleSource source)
{
    state.source = source;
    recomp_scene_freeze_set_frame_hook(sample_while_frozen);
}

/* Handles the toggles in one sample of port 0. True when the sample is the
   free camera's rather than the game's. */
static bool take_sample(const RecompInputGamepad *gamepad)
{
    bool toggle = recomp_input_host_key(RECOMP_INPUT_HOST_KEY_F1) ||
        ((gamepad->buttons & XBOX_BACK) != 0u && (gamepad->buttons & XBOX_RIGHT_THUMB) != 0u);
    if (toggle && !state.toggle_held) set_active(!state.active);
    state.toggle_held = toggle;

    bool freeze = state.active && (recomp_input_host_key(RECOMP_INPUT_HOST_KEY_P) ||
        (gamepad->buttons & XBOX_START) != 0u);
    if (freeze && !state.freeze_held) recomp_scene_freeze_set(!recomp_scene_freeze_active());
    state.freeze_held = freeze;

    bool overlay = state.active && (recomp_input_host_key(RECOMP_INPUT_HOST_KEY_H) ||
        gamepad->analog_buttons[ANALOG_BLACK] >= ANALOG_PRESSED);
    if (overlay && !state.overlay_held) {
        /* A clean view hides the game's overlay and the runtime's panel
           together; the next press shows both. */
        bool hide = !state.panel_hidden || !recomp_scene_freeze_overlay_hidden();
        state.panel_hidden = hide;
        recomp_scene_freeze_set_overlay_hidden(hide);
    }
    state.overlay_held = overlay;

    bool step = state.active && (recomp_input_host_key(RECOMP_INPUT_HOST_KEY_PERIOD) ||
        gamepad->analog_buttons[ANALOG_WHITE] >= ANALOG_PRESSED);
    if (step && !state.step_held) recomp_scene_freeze_step();
    state.step_held = step;

    bool clip_back = state.active && (recomp_input_host_key(RECOMP_INPUT_HOST_KEY_LEFT_BRACKET) ||
        (gamepad->buttons & XBOX_DPAD_LEFT) != 0u);
    bool clip_next = state.active && (recomp_input_host_key(RECOMP_INPUT_HOST_KEY_RIGHT_BRACKET) ||
        (gamepad->buttons & XBOX_DPAD_RIGHT) != 0u);
    int clip_step = (clip_next && !state.clip_next_held) - (clip_back && !state.clip_back_held);
    if (clip_step != 0 && recomp_actor_pose_step_clip(clip_step) && recomp_scene_freeze_active()) {
        recomp_scene_freeze_step();
    }
    state.clip_back_held = clip_back;
    state.clip_next_held = clip_next;

    bool actor = state.active && (recomp_input_host_key(RECOMP_INPUT_HOST_KEY_BACKSLASH) ||
        (gamepad->buttons & XBOX_LEFT_THUMB) != 0u);
    if (actor && !state.actor_held) {
        recomp_actor_pose_next_actor();
        state.aim_pending = true;
    }
    state.actor_held = actor;

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
    controls->zoom = ((pad->buttons & XBOX_DPAD_UP) ? 1.0f : 0.0f) -
        ((pad->buttons & XBOX_DPAD_DOWN) ? 1.0f : 0.0f) +
        key(RECOMP_INPUT_HOST_KEY_Z, RECOMP_INPUT_HOST_KEY_X);
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

static void build_camera(void)
{
    const uint32_t object = recomp_runtime.registers.eax;
    const uint32_t frame = recomp_d3d_frame_adapter_swap_counter();
    const uint32_t slot = (object - CAMERA_FIRST) / CAMERA_STRIDE;
    const bool known = object >= CAMERA_FIRST && slot < CAMERA_COUNT &&
        object == CAMERA_FIRST + slot * CAMERA_STRIDE;

    if (state.built_frame != frame) {
        recomp_actor_pose_frame();
        state.built_before = state.built;
        state.built = 0u;
        state.built_frame = frame;
    }
    if (known) state.built |= 1u << slot;
    if (state.active) {
        bool cycle = analog(&state.pad, ANALOG_X) || recomp_input_host_key(RECOMP_INPUT_HOST_KEY_C);
        if (cycle && !state.cycle_held) {
            for (uint32_t step = 1u; step <= CAMERA_COUNT; ++step) {
                uint32_t next = (state.slot + step) % CAMERA_COUNT;
                if ((state.built_before & (1u << next)) != 0u) {
                    if (next != state.slot) {
                        state.slot = next;
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
        fprintf(stderr, "recomp free camera trace: frame=%u object=%08x active=%d "
            "eye=%.3f,%.3f,%.3f target=%.3f,%.3f,%.3f fov=%.4f\n",
            frame, object, state.active && ours, eye[0], eye[1], eye[2], target[0], target[1], target[2], fov);
    }

    const bool fly = state.active && ours;
    if (fly) {
        bool reset = analog(&state.pad, ANALOG_Y) || recomp_input_host_key(RECOMP_INPUT_HOST_KEY_R);
        if ((!state.started || (reset && !state.reset_held)) &&
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

    if (fly && state.started) {
        recomp_guest_store(object + CAMERA_EYE, eye, sizeof eye);
        recomp_guest_store(object + CAMERA_TARGET, target, sizeof target);
        recomp_guest_store(object + CAMERA_FOV, &fov, sizeof fov);
    }
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
}

void recomp_free_camera_overlay(bool viewer)
{
    RecompOverlayText *text = recomp_overlay_shown();
    recomp_overlay_clear(text);
    if (!state.active || state.panel_hidden) return;

    RecompActorPoseStatus pose;
    char name[48];
    recomp_actor_pose_status(&pose);
    recomp_overlay_set_title(text, viewer ? "MODEL VIEWER" : "FREE CAMERA");
    if (pose.active) {
        recomp_actor_pose_name(pose.actor, name, sizeof name);
        recomp_overlay_add_line(text, "%s", name);
        if (pose.holding) {
            recomp_overlay_add_line(text, "Animation %u of %u (clip %04X)",
                (unsigned)pose.position, (unsigned)pose.count, (unsigned)pose.clip);
        } else {
            recomp_overlay_add_line(text, "Animation: the game's own");
        }
    }
    recomp_overlay_add_line(text, "%s", recomp_scene_freeze_active() ? "Frozen" : "Running");
    recomp_overlay_set_hint(text, viewer
        ? "F1 leave   P freeze   . step   H clean view\n"
          "WASD QE move   arrows / right-drag look   [ ] animation   \\ next girl"
        : "F1 off   P freeze   . step   H clean view\n"
          "WASD QE move   arrows / right-drag look   [ ] animation   \\ next girl");
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
