/* Switch host for recomp_program_runner: d3d_presenter_gl.cpp draws, audio
   is silent and no controller is connected. The log is shown on screen until
   the presenter takes the window, and is always copied to /doaxbv/log.txt.
   shortcut: no audio or input yet; replace each with a libnx backend behind
   the same interface. */
#define _POSIX_C_SOURCE 200809L /* setenv under the strict C11 build */
#include "audio_output.h"
#include "input_host_win32.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <unwind.h>
#include <switch.h>

void recomp_switch_console_exit(void);

/* ── Audio: no device, so guest audio timing stays as it is ── */

void recomp_audio_output_initialize(void) {}
void recomp_audio_output_shutdown(void) {}
void recomp_audio_output_reset_voice(uint32_t slot) { (void)slot; }
int recomp_audio_output_enabled(void) { return 0; }

void recomp_audio_output_submit(
    uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
    int32_t volume_hundredth_db)
{
    (void)slot; (void)pcm; (void)bytes; (void)sample_rate;
    (void)channels; (void)bits_per_sample; (void)volume_hundredth_db;
}

/* ── Input: no controller connected ── */

bool recomp_input_host_sample(uint32_t port, RecompInputGamepad *gamepad)
{
    (void)port;
    (void)gamepad;
    return false;
}

void recomp_input_host_set_vibration(uint32_t port, uint16_t left_motor, uint16_t right_motor)
{
    (void)port; (void)left_motor; (void)right_motor;
}

void recomp_input_host_stop_vibration(void) {}

/* ── Entry: arguments from a file, log on screen and on the SD card ── */

/* A rooted path: the runner's std::filesystem::absolute treats "sdmc:/" as
   relative, and libnx maps "/" to the SD card. */
#define SWITCH_ROOT "/doaxbv"

int recomp_runner_main(int argc, char **argv);

static FILE *g_log;
static const devoptab_t *g_console;
static devoptab_t g_tee;
static PadState g_pad;
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
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) break;
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
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    g_log = fopen(SWITCH_ROOT "/log.txt", "w");
    g_console = devoptab_list[STD_OUT];
    g_tee = *g_console;
    g_tee.write_r = tee_write;
    devoptab_list[STD_OUT] = &g_tee;
    devoptab_list[STD_ERR] = &g_tee;
    atexit(hold_until_plus);
    signal(SIGABRT, on_abort);

    argc = read_arguments(argv, 39);
    argv[argc] = NULL;
    printf("DOAXBV Switch runner: %s\n", argv[2]);
    code = recomp_runner_main(argc, argv);
    printf("Runner returned %d.\n", code);
    return code;
}
