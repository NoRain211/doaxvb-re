#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "stop_report.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    MAX_STOP_ID = 160,
    MAX_PATH = 512,
};

static char expected_stop[MAX_STOP_ID];
static char configured_boundary[MAX_STOP_ID];
static char milestone_path[MAX_PATH];
static int have_expectation;
static int boundary_reached;
static unsigned long kernel_calls;

void recomp_stop_configure(const char *expected, const char *milestone_log)
{
    if (expected != NULL && expected[0] != '\0') {
        snprintf(expected_stop, sizeof expected_stop, "%s", expected);
        have_expectation = 1;
    }
    if (milestone_log != NULL && milestone_log[0] != '\0') {
        snprintf(milestone_path, sizeof milestone_path, "%s", milestone_log);
    }
}

void recomp_stop_configure_boundary(const char *stop_id)
{
    if (stop_id != NULL && stop_id[0] != '\0') {
        snprintf(
            configured_boundary, sizeof configured_boundary, "%s", stop_id);
    }
}

void recomp_stop_at_boundary(const char *stop_id)
{
    if (configured_boundary[0] != '\0' && stop_id != NULL &&
        strcmp(stop_id, configured_boundary) == 0) {
        boundary_reached = 1;
        recomp_stop(0, "%s", stop_id);
    }
}

void recomp_stop_note_kernel_call(void)
{
    ++kernel_calls;
}

static void append_milestone(
    const char *stop_id, const char *result, const char *kind,
    int fallback_code, int exit_code)
{
    FILE *log;
    char stamp[32];
    time_t now;
    struct tm parts;

    if (milestone_path[0] == '\0') {
        return;
    }
    log = fopen(milestone_path, "a");
    if (log == NULL) {
        fprintf(
            stderr,
            "recomp stop: cannot append milestone log %s\n",
            milestone_path);
        return;
    }

    now = time(NULL);
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &parts);

    fprintf(
        log,
        "%s\tstop=%s\texpected=%s\tresult=%s\tkernel_calls=%lu"
        "\tevent=runtime-stop\tkind=%s\tfallback_code=%d\texit_code=%d\n",
        stamp,
        stop_id,
        have_expectation ? expected_stop : "-",
        result,
        kernel_calls, kind, fallback_code, exit_code);
    fclose(log);
}

void recomp_stop(int fallback_code, const char *stop_id_format, ...)
{
    char stop_id[MAX_STOP_ID];
    va_list args;
    int matched;
    int code;
    const char *result;
    const char *kind;

    va_start(args, stop_id_format);
    vsnprintf(stop_id, sizeof stop_id, stop_id_format, args);
    va_end(args);

    /* Prefix match, so an expectation can name a family ("import:") or an
       exact stop ("import:NtOpenFile"). */
    matched = have_expectation &&
        strncmp(stop_id, expected_stop, strlen(expected_stop)) == 0;
    code = have_expectation ? (matched ? 0 : 3) : fallback_code;
    result = have_expectation ? (matched ? "match" : "mismatch") : "unchecked";

    kind = boundary_reached ? "diagnostic-boundary"
        : fallback_code == 0 ? "normal-exit" : "runtime-error";
    append_milestone(stop_id, result, kind, fallback_code, code);

    fprintf(
        stderr,
        "recomp stop: %s expected=%s result=%s kernel_calls=%lu"
        " event=runtime-stop kind=%s fallback_code=%d exit_code=%d\n",
        stop_id, have_expectation ? expected_stop : "-", result,
        kernel_calls, kind, fallback_code, code);
    fflush(stderr);
    exit(code);
}
