/* Switch host for recomp_program_runner: d3d_presenter_gl.cpp draws,
   audio_output_switch.cpp plays sound, and this file reads the controllers.
   The log is shown on screen until the presenter takes the window, and is
   always copied to /doaxbv/log.txt. */
#define _POSIX_C_SOURCE 200809L /* setenv under the strict C11 build */
#include "input_host_win32.h"
#include "input_pulse_source.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <unwind.h>
#include <switch.h>

void recomp_switch_console_exit(void);

/* ── Input: player N drives guest port N; port 0 also takes handheld mode ── */

enum {
    XBOX_DPAD_UP = 0x0001u,
    XBOX_DPAD_DOWN = 0x0002u,
    XBOX_DPAD_LEFT = 0x0004u,
    XBOX_DPAD_RIGHT = 0x0008u,
    XBOX_START = 0x0010u,
    XBOX_BACK = 0x0020u,
    XBOX_LEFT_THUMB = 0x0040u,
    XBOX_RIGHT_THUMB = 0x0080u,
};

static PadState g_pads[RECOMP_INPUT_PORT_COUNT];

static void initialize_pads(void)
{
    padConfigureInput(RECOMP_INPUT_PORT_COUNT, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pads[0]);
    for (uint32_t port = 1u; port < RECOMP_INPUT_PORT_COUNT; ++port) {
        padInitialize(&g_pads[port], (HidNpadIdType)(HidNpadIdType_No1 + port));
    }
}

bool recomp_input_host_sample(uint32_t port, RecompInputGamepad *gamepad)
{
    /* Buttons map by label, so A confirms and B goes back as on the Xbox. */
    static const struct { u64 button; uint16_t mask; } digital[] = {
        {HidNpadButton_Up, XBOX_DPAD_UP},
        {HidNpadButton_Down, XBOX_DPAD_DOWN},
        {HidNpadButton_Left, XBOX_DPAD_LEFT},
        {HidNpadButton_Right, XBOX_DPAD_RIGHT},
        {HidNpadButton_Plus, XBOX_START},
        {HidNpadButton_Minus, XBOX_BACK},
        {HidNpadButton_StickL, XBOX_LEFT_THUMB},
        {HidNpadButton_StickR, XBOX_RIGHT_THUMB},
    };
    static const struct { u64 button; unsigned index; } pressure[] = {
        {HidNpadButton_A, RECOMP_INPUT_ANALOG_A},
        {HidNpadButton_B, RECOMP_INPUT_ANALOG_B},
        {HidNpadButton_X, RECOMP_INPUT_ANALOG_X},
        {HidNpadButton_Y, RECOMP_INPUT_ANALOG_Y},
        {HidNpadButton_L, RECOMP_INPUT_ANALOG_BLACK},
        {HidNpadButton_R, RECOMP_INPUT_ANALOG_WHITE},
        {HidNpadButton_ZL, RECOMP_INPUT_ANALOG_LTRIG},
        {HidNpadButton_ZR, RECOMP_INPUT_ANALOG_RTRIG},
    };
    if (gamepad == NULL || port >= RECOMP_INPUT_PORT_COUNT) return false;
    memset(gamepad, 0, sizeof *gamepad);
    PadState *pad = &g_pads[port];
    padUpdate(pad);
    if (!padIsConnected(pad)) return false;

    u64 held = padGetButtons(pad);
    for (size_t i = 0; i < sizeof digital / sizeof digital[0]; ++i) {
        if (held & digital[i].button) gamepad->buttons |= digital[i].mask;
    }
    for (size_t i = 0; i < sizeof pressure / sizeof pressure[0]; ++i) {
        if (held & pressure[i].button) gamepad->analog_buttons[pressure[i].index] = 0xffu;
    }
    /* Both sticks report -32767..32767 with up positive, as XInput does. */
    HidAnalogStickState left = padGetStickPos(pad, 0), right = padGetStickPos(pad, 1);
    gamepad->thumb_lx = (int16_t)left.x;
    gamepad->thumb_ly = (int16_t)left.y;
    gamepad->thumb_rx = (int16_t)right.x;
    gamepad->thumb_ry = (int16_t)right.y;
    return true;
}

/* The game resends both motors every frame, so each value just replaces
   the last. Left (heavy) drives the low band, right the high band. */
void recomp_input_host_set_vibration(uint32_t port, uint16_t left_motor, uint16_t right_motor)
{
    static HidVibrationDeviceHandle handles[RECOMP_INPUT_PORT_COUNT][2];
    static u32 handle_key[RECOMP_INPUT_PORT_COUNT];
    if (port >= RECOMP_INPUT_PORT_COUNT) return;
    const PadState *pad = &g_pads[port];
    u32 styles = padGetStyleSet(pad);
    HidNpadIdType id = port == 0u && padIsHandheld(pad) ?
        HidNpadIdType_Handheld : (HidNpadIdType)(HidNpadIdType_No1 + port);
    HidNpadStyleTag style = id == HidNpadIdType_Handheld ? HidNpadStyleTag_NpadHandheld :
        (styles & HidNpadStyleTag_NpadFullKey) ? HidNpadStyleTag_NpadFullKey :
        (styles & HidNpadStyleTag_NpadJoyDual) ? HidNpadStyleTag_NpadJoyDual : 0;
    if (style == 0) return;
    u32 key = (u32)id << 16 | (u32)style; /* nonzero once initialized */
    if (handle_key[port] != key) {
        if (R_FAILED(hidInitializeVibrationDevices(handles[port], 2, id, style))) return;
        handle_key[port] = key;
    }
    HidVibrationValue value = {
        .amp_low = left_motor / 65535.0f, .freq_low = 160.0f,
        .amp_high = right_motor / 65535.0f, .freq_high = 320.0f,
    };
    HidVibrationValue values[2] = {value, value};
    hidSendVibrationValues(handles[port], values, 2);
}

void recomp_input_host_stop_vibration(void)
{
    for (uint32_t port = 0u; port < RECOMP_INPUT_PORT_COUNT; ++port) {
        recomp_input_host_set_vibration(port, 0u, 0u);
    }
}

/* No keyboard or mouse: the free camera and screenshots use controller combos. */
bool recomp_input_host_key(RecompInputHostKey key)
{
    (void)key;
    return false;
}

bool recomp_input_host_mouse_drag(float *dx, float *dy)
{
    *dx = *dy = 0.0f;
    return false;
}

/* ── Entry: arguments from a file, log on screen and on the SD card ── */

/* A rooted path: the runner's std::filesystem::absolute treats "sdmc:/" as
   relative, and libnx maps "/" to the SD card. */
#define SWITCH_ROOT "/doaxbv"

int recomp_runner_main(int argc, char **argv);

static FILE *g_log;
static const devoptab_t *g_console;
static devoptab_t g_tee;
static Handle g_main_thread;
static u64 g_last_refresh;
static bool g_console_active;

/* The GL presenter takes over the window the text console draws into; from
   then on the log goes only to the SD card. */
void recomp_switch_console_exit(void)
{
    if (!g_console_active) return;
    g_console_active = false;
    consoleExit(NULL);
}

/* libnx aborts if another thread presents the console, so only the thread
   that runs the game refreshes it, at most ten times a second. */
static void refresh_screen(void)
{
    u64 now = armGetSystemTick();
    if (!g_console_active || threadGetCurHandle() != g_main_thread ||
        armTicksToNs(now - g_last_refresh) < 100000000ULL) {
        return;
    }
    g_last_refresh = now;
    consoleUpdate(NULL);
}

static ssize_t tee_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    ssize_t written = g_console_active ? g_console->write_r(r, fd, ptr, len) : (ssize_t)len;
    if (g_log != NULL) {
        fwrite(ptr, 1, len, g_log);
        fflush(g_log);
    }
    refresh_screen();
    return written;
}

/* Keeps the last lines readable after the runner stops. */
static void hold_until_plus(void)
{
    if (!g_console_active) { /* nothing left on screen to read */
        if (g_log != NULL) fclose(g_log);
        g_log = NULL;
        return;
    }
    printf("\nRunner stopped. Press + to quit.\n");
    fflush(stdout);
    while (appletMainLoop()) {
        padUpdate(&g_pads[0]);
        if (padGetButtonsDown(&g_pads[0]) & HidNpadButton_Plus) break;
        refresh_screen();
        svcSleepThread(16000000LL);
    }
    if (g_log != NULL) fclose(g_log);
    g_log = NULL;
}

static _Unwind_Reason_Code print_frame(struct _Unwind_Context *context, void *arg)
{
    (void)arg;
    printf("  at %p\n", (void *)_Unwind_GetIP(context));
    return _URC_NO_REASON;
}

int main(void);

/* abort() and std::terminate leave through _exit, which skips atexit, so
   without this the program just vanishes. */
static void on_abort(int signal_number)
{
    (void)signal_number;
    /* addr2line offset = address - (main's address - main's .elf address) */
    printf("\nAborted. main is at %p. Backtrace:\n", (void *)main);
    _Unwind_Backtrace(print_frame, NULL);
    hold_until_plus();
}

/* One argument per line of args.txt; KEY=VALUE lines set the environment. */
static int read_arguments(char **argv, int capacity)
{
    static char lines[32][512];
    int argc = 0, count = 0;
    FILE *file = fopen(SWITCH_ROOT "/args.txt", "r");

    argv[argc++] = "recomp_program_runner";
    argv[argc++] = "--xbe";
    argv[argc++] = SWITCH_ROOT "/game/default.xbe";
    if (file == NULL) return argc;
    while (count < 32 && argc < capacity && fgets(lines[count], sizeof lines[count], file) != NULL) {
        char *line = lines[count++];
        char *equals;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        equals = strchr(line, '=');
        if (line[0] != '-' && equals != NULL) {
            *equals = '\0';
            setenv(line, equals + 1, 1);
        } else {
            argv[argc++] = line;
        }
    }
    fclose(file);
    return argc;
}

int main(void)
{
    char *argv[40];
    int argc, code;

    g_main_thread = threadGetCurHandle();
    consoleInit(NULL);
    g_console_active = true;
    consoleDebugInit(debugDevice_CONSOLE); /* stderr goes nowhere by default */
    initialize_pads();
    g_log = fopen(SWITCH_ROOT "/log.txt", "w");
    g_console = devoptab_list[STD_OUT];
    g_tee = *g_console;
    g_tee.write_r = tee_write;
    devoptab_list[STD_OUT] = &g_tee;
    devoptab_list[STD_ERR] = &g_tee;
    atexit(hold_until_plus);
    signal(SIGABRT, on_abort);

    setenv("RECOMP_AUDIO_GAIN", "1", 0); /* full volume, as run_game.py; args.txt may override */
    setenv("RECOMP_SCREENSHOT_DIR", SWITCH_ROOT "/screenshots", 0);
    argc = read_arguments(argv, 39);
    argv[argc] = NULL;
    printf("DOAXBV Switch runner: %s\n", argv[2]);
    code = recomp_runner_main(argc, argv);
    printf("Runner returned %d.\n", code);
    return code;
}
