/* Switch entry for recomp_runtime_test. An NRO gets no command line, so this
   runs the default suite, shows the result on screen and writes the exit code
   to sdmc:/doaxbv-runtime-test.txt. */
#include <stdio.h>
#include <switch.h>

int recomp_runtime_test_main(int argc, char **argv);

int main(void)
{
    char name[] = "recomp_runtime_test";
    char *argv[] = {name, NULL};
    PadState pad;
    int code;
    FILE *result;

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    printf("Running runtime tests...\n");
    consoleUpdate(NULL);

    code = recomp_runtime_test_main(1, argv);
    result = fopen("sdmc:/doaxbv-runtime-test.txt", "w");
    if (result != NULL) {
        fprintf(result, "exit %d\n", code);
        fclose(result);
    }
    printf("\n%s (exit %d). Press + to quit.\n", code == 0 ? "PASS" : "FAIL", code);

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return code;
}
