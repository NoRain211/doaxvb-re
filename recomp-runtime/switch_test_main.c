/* Switch entry for recomp_runtime_test. An NRO gets no command line, so this
   runs the default suite and shows the result on screen. The screen holds
   only the last lines, so all output is also kept in
   sdmc:/doaxbv-runtime-test.txt. */
#include <stdio.h>
#include <sys/stat.h>
#include <sys/iosupport.h>
#include <switch.h>

int recomp_runtime_test_main(int argc, char **argv);

static FILE *g_log;
static const devoptab_t *g_console;
static devoptab_t g_tee;

static ssize_t tee_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    fwrite(ptr, 1, len, g_log);
    return g_console->write_r(r, fd, ptr, len);
}

int main(void)
{
    char name[] = "recomp_runtime_test";
    char *argv[] = {name, NULL};
    PadState pad;
    int code;

    consoleInit(NULL);
    consoleDebugInit(debugDevice_CONSOLE); /* stderr goes nowhere by default. */
    g_log = fopen("sdmc:/doaxbv-runtime-test.txt", "w");
    if (g_log != NULL) {
        g_console = devoptab_list[STD_OUT];
        g_tee = *g_console;
        g_tee.write_r = tee_write;
        devoptab_list[STD_OUT] = &g_tee;
        devoptab_list[STD_ERR] = &g_tee;
    }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    mkdir("/tmp", 0777); /* GetTempPathA's default; the SD card has none. */
    printf("Running runtime tests...\n");
    consoleUpdate(NULL);

    code = recomp_runtime_test_main(1, argv);
    printf("\n%s (exit %d). Press + to quit.\n", code == 0 ? "PASS" : "FAIL", code);
    fflush(stdout);
    fflush(stderr);
    if (g_log != NULL) {
        /* Eden writes the file out only when it is closed. */
        devoptab_list[STD_OUT] = g_console;
        devoptab_list[STD_ERR] = g_console;
        fclose(g_log);
    }

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return code;
}
