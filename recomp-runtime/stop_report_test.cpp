#include "stop_report.h"
#include "host_diagnostics.h"
#include "runtime.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Diagnostics read these only for Debug context; this fixture runs no guest. */
RecompRuntime recomp_runtime{};
uint32_t recomp_last_dispatch_address;

#ifdef _MSC_VER
#pragma warning(disable : 4717) // the recursion is the test
#endif
static int overflow(int depth)
{
    volatile char frame[4096];
    frame[0] = static_cast<char>(depth);
    return overflow(depth + 1) + frame[0];
}

int main(int argc, char **argv)
{
    if (argc != 3) return 64;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    recomp_install_host_diagnostics();
    const char *mode = argv[1];
    recomp_stop_configure(nullptr, argv[2]);
    if (std::strcmp(mode, "normal") == 0) recomp_stop(0, "host-window-close");
    if (std::strcmp(mode, "mismatch") == 0) {
        recomp_stop_configure("missing-body:", nullptr);
        recomp_stop(0, "host-console-break");
    }
    if (std::strcmp(mode, "expected-error") == 0) {
        recomp_stop_configure("missing-body:", nullptr);
        recomp_stop(2, "missing-body:test");
    }
    if (std::strcmp(mode, "error") == 0) recomp_stop(2, "missing-body:test");
    if (std::strcmp(mode, "boundary") == 0) {
        recomp_stop_configure_boundary("test-boundary");
        recomp_stop_at_boundary("test-boundary");
    }
    if (std::strcmp(mode, "handled") == 0) {
        __try {
            RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, 0, 0, nullptr);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
        }
        recomp_stop(0, "completed");
    }
    if (std::strcmp(mode, "crash") == 0) {
        recomp_stop_configure("completed", nullptr);
        RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, 0, 0, nullptr);
    }
    if (std::strcmp(mode, "overflow") == 0) return overflow(0);
    if (std::strcmp(mode, "abort") == 0) std::abort();
    if (std::strcmp(mode, "invalid-parameter") == 0) return std::fclose(nullptr);
    return 65;
}
