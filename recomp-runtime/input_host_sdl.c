// SPDX-License-Identifier: GPL-3.0-or-later
#include "input_host_win32.h"
#include "input_pulse_source.h"

#include <SDL3/SDL.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    XBOX_DPAD_UP = 0x0001u,
    XBOX_DPAD_DOWN = 0x0002u,
    XBOX_DPAD_LEFT = 0x0004u,
    XBOX_DPAD_RIGHT = 0x0008u,
    XBOX_START = 0x0010u,
    XBOX_BACK = 0x0020u,
    XBOX_LEFT_THUMB = 0x0040u,
    XBOX_RIGHT_THUMB = 0x0080u,
    /* The game resends motors every frame; a stalled game stops rumbling. */
    RUMBLE_DURATION_MS = 1000u,
};

static SDL_Gamepad *pads[RECOMP_INPUT_PORT_COUNT];

static bool trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("RECOMP_INPUT_TRACE") != NULL;
    return enabled != 0;
}

/* SDL starts lazily on the guest thread that polls it. There is no SDL event
   loop; SDL_UpdateGamepads pumps it. A new gamepad takes the lowest free port
   and keeps it until it disconnects. */
static void update_pads(void)
{
    static bool initialized;

    if (!initialized) {
        initialized = true;
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_Init(SDL_INIT_GAMEPAD)) {
            fprintf(stderr, "recomp input: SDL gamepad init failed: %s\n", SDL_GetError());
        }
        SDL_SetJoystickEventsEnabled(false);
        SDL_SetGamepadEventsEnabled(false);
    }
    SDL_UpdateGamepads();
    for (uint32_t port = 0u; port < RECOMP_INPUT_PORT_COUNT; ++port) {
        if (pads[port] != NULL && !SDL_GamepadConnected(pads[port])) {
            SDL_CloseGamepad(pads[port]);
            pads[port] = NULL;
        }
    }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != NULL && i < count; ++i) {
        uint32_t free_port = RECOMP_INPUT_PORT_COUNT;
        bool open = false;
        for (uint32_t port = 0u; port < RECOMP_INPUT_PORT_COUNT; ++port) {
            if (pads[port] == NULL) {
                if (free_port == RECOMP_INPUT_PORT_COUNT) free_port = port;
            } else if (SDL_GetGamepadID(pads[port]) == ids[i]) {
                open = true;
            }
        }
        if (!open && free_port < RECOMP_INPUT_PORT_COUNT) {
            pads[free_port] = SDL_OpenGamepad(ids[i]);
            if (pads[free_port] != NULL) {
                fprintf(stderr, "recomp input: gamepad '%s' on port=%u\n",
                    SDL_GetGamepadName(pads[free_port]), (unsigned)free_port);
            }
        }
    }
    SDL_free(ids);
}

void recomp_input_host_set_vibration(
    uint32_t port,
    uint16_t left_motor,
    uint16_t right_motor)
{
    static uint32_t traced[RECOMP_INPUT_PORT_COUNT];
    uint32_t motors = (uint32_t)left_motor << 16 | right_motor;

    if (port >= RECOMP_INPUT_PORT_COUNT || pads[port] == NULL) {
        return;
    }
    /* SDL sends only changed values and keeps a held value alive. */
    bool sent = SDL_RumbleGamepad(pads[port], left_motor, right_motor, RUMBLE_DURATION_MS);
    if (trace_enabled() && traced[port] != motors) {
        traced[port] = motors;
        fprintf(stderr, "recomp input: vibration port=%u left=%u right=%u sent=%u\n",
            (unsigned)port, (unsigned)left_motor, (unsigned)right_motor, (unsigned)sent);
    }
}

void recomp_input_host_stop_vibration(void)
{
    for (uint32_t port = 0u; port < RECOMP_INPUT_PORT_COUNT; ++port) {
        recomp_input_host_set_vibration(port, 0u, 0u);
    }
}

static void trace_sample(
    const RecompInputGamepad *pad, SDL_Gamepad *host, bool focused)
{
    static bool seen;
    static unsigned lines;
    static RecompInputGamepad previous;
    static SDL_JoystickID previous_id;
    static bool previous_focused;
    if (!trace_enabled() || lines >= 4096u) return;
    SDL_JoystickID id = host != NULL ? SDL_GetGamepadID(host) : 0;
    if (seen && memcmp(&previous, pad, sizeof previous) == 0 &&
        previous_id == id && previous_focused == focused) return;
    const char *host_name = host != NULL ? SDL_GetGamepadName(host) : NULL;
    fprintf(stderr,
        "[DEBUG-r501-input] tick_ms=%llu pad='%s' focused=%u"
        " digital=%04x analog=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x"
        " axes=%d,%d,%d,%d\n",
        (unsigned long long)SDL_GetTicks(), host_name != NULL ? host_name : "none",
        focused ? 1u : 0u,
        (unsigned)pad->buttons, pad->analog_buttons[0], pad->analog_buttons[1],
        pad->analog_buttons[2], pad->analog_buttons[3], pad->analog_buttons[4],
        pad->analog_buttons[5], pad->analog_buttons[6], pad->analog_buttons[7],
        pad->thumb_lx, pad->thumb_ly, pad->thumb_rx, pad->thumb_ry);
    previous = *pad;
    previous_id = id;
    previous_focused = focused;
    seen = true;
    ++lines;
}

static uint8_t button_pressure(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
    return SDL_GetGamepadButton(gamepad, button) ? 0xffu : 0u;
}

/* SDL Y axes point down; XInput Y axes point up. */
static int16_t up_axis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
    int16_t down = SDL_GetGamepadAxis(gamepad, axis);
    return down == INT16_MIN ? INT16_MAX : (int16_t)-down;
}

static void read_pad(SDL_Gamepad *host, RecompInputGamepad *gamepad)
{
    static const struct { SDL_GamepadButton button; uint16_t mask; } digital[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, XBOX_DPAD_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XBOX_DPAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XBOX_DPAD_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XBOX_DPAD_RIGHT},
        {SDL_GAMEPAD_BUTTON_START, XBOX_START},
        {SDL_GAMEPAD_BUTTON_BACK, XBOX_BACK},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, XBOX_LEFT_THUMB},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XBOX_RIGHT_THUMB},
    };
    for (size_t i = 0; i < sizeof digital / sizeof digital[0]; ++i) {
        if (SDL_GetGamepadButton(host, digital[i].button)) {
            gamepad->buttons |= digital[i].mask;
        }
    }
    /* SDL names face buttons by position, so Cross/Circle/Square/Triangle
       land on the Xbox A/B/X/Y slots. */
    gamepad->analog_buttons[RECOMP_INPUT_ANALOG_A] = button_pressure(host, SDL_GAMEPAD_BUTTON_SOUTH);
    gamepad->analog_buttons[RECOMP_INPUT_ANALOG_B] = button_pressure(host, SDL_GAMEPAD_BUTTON_EAST);
    gamepad->analog_buttons[RECOMP_INPUT_ANALOG_X] = button_pressure(host, SDL_GAMEPAD_BUTTON_WEST);
    gamepad->analog_buttons[RECOMP_INPUT_ANALOG_Y] = button_pressure(host, SDL_GAMEPAD_BUTTON_NORTH);
    gamepad->analog_buttons[RECOMP_INPUT_ANALOG_BLACK] =
        button_pressure(host, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    gamepad->analog_buttons[RECOMP_INPUT_ANALOG_WHITE] =
        button_pressure(host, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    /* SDL triggers are 0..32767; the guest expects 0..255. */
    gamepad->analog_buttons[RECOMP_INPUT_ANALOG_LTRIG] =
        (uint8_t)(SDL_GetGamepadAxis(host, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    gamepad->analog_buttons[RECOMP_INPUT_ANALOG_RTRIG] =
        (uint8_t)(SDL_GetGamepadAxis(host, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
    gamepad->thumb_lx = SDL_GetGamepadAxis(host, SDL_GAMEPAD_AXIS_LEFTX);
    gamepad->thumb_ly = up_axis(host, SDL_GAMEPAD_AXIS_LEFTY);
    gamepad->thumb_rx = SDL_GetGamepadAxis(host, SDL_GAMEPAD_AXIS_RIGHTX);
    gamepad->thumb_ry = up_axis(host, SDL_GAMEPAD_AXIS_RIGHTY);
}

bool recomp_input_host_sample(uint32_t port, RecompInputGamepad *gamepad)
{
    if (gamepad == NULL || port >= RECOMP_INPUT_PORT_COUNT) {
        return false;
    }
    memset(gamepad, 0, sizeof *gamepad);
    update_pads();
    SDL_Gamepad *host = pads[port];
    if (host != NULL) {
        read_pad(host, gamepad);
    }
    if (port != 0u) {
        /* The keyboard drives port 0 only. */
        return host != NULL;
    }

    const bool *keyboard = SDL_GetKeyboardState(NULL);
    SDL_Window *focus_win = SDL_GetKeyboardFocus();
    bool focused = (focus_win != NULL && (SDL_GetWindowFlags(focus_win) & SDL_WINDOW_INPUT_FOCUS) != 0);
    if (!focused || keyboard == NULL) {
        trace_sample(gamepad, host, focused);
        return true;
    }

    if (keyboard[SDL_SCANCODE_UP]) gamepad->buttons |= XBOX_DPAD_UP;
    if (keyboard[SDL_SCANCODE_DOWN]) gamepad->buttons |= XBOX_DPAD_DOWN;
    if (keyboard[SDL_SCANCODE_LEFT]) gamepad->buttons |= XBOX_DPAD_LEFT;
    if (keyboard[SDL_SCANCODE_RIGHT]) gamepad->buttons |= XBOX_DPAD_RIGHT;
    if (keyboard[SDL_SCANCODE_RETURN]) gamepad->buttons |= XBOX_START;
    if (keyboard[SDL_SCANCODE_BACKSPACE]) gamepad->buttons |= XBOX_BACK;

    if (keyboard[SDL_SCANCODE_A]) gamepad->analog_buttons[RECOMP_INPUT_ANALOG_X] = 0xffu;
    if (keyboard[SDL_SCANCODE_S]) gamepad->analog_buttons[RECOMP_INPUT_ANALOG_Y] = 0xffu;
    if (keyboard[SDL_SCANCODE_Z]) gamepad->analog_buttons[RECOMP_INPUT_ANALOG_A] = 0xffu;
    if (keyboard[SDL_SCANCODE_X]) gamepad->analog_buttons[RECOMP_INPUT_ANALOG_B] = 0xffu;
    if (keyboard[SDL_SCANCODE_Q]) gamepad->analog_buttons[RECOMP_INPUT_ANALOG_WHITE] = 0xffu;
    if (keyboard[SDL_SCANCODE_W]) gamepad->analog_buttons[RECOMP_INPUT_ANALOG_BLACK] = 0xffu;
    if (keyboard[SDL_SCANCODE_E]) gamepad->analog_buttons[RECOMP_INPUT_ANALOG_LTRIG] = 0xffu;
    if (keyboard[SDL_SCANCODE_R]) gamepad->analog_buttons[RECOMP_INPUT_ANALOG_RTRIG] = 0xffu;
    trace_sample(gamepad, host, true);
    return true;
}
