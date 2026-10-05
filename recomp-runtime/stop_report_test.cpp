#include "stop_report.h"
#include "host_diagnostics.h"
#include "runtime.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#endif
#include <cstring>

/* Diagnostics read these only for Debug context; this fixture runs no guest. */
RecompRuntime recomp_runtime{};
uint32_t recomp_last_dispatch_address;

int main(int argc, char **argv)
{
    if (argc != 3) return 64;
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
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
#ifdef _WIN32
        __try {
            RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, 0, 0, nullptr);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
        }
#else
        struct sigaction action{}, previous{};
        action.sa_handler = [](int) {};
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGFPE, &action, &previous) != 0) return 66;
        std::raise(SIGFPE);
        if (sigaction(SIGFPE, &previous, nullptr) != 0) return 66;
#endif
        recomp_stop(0, "completed");
    }
    if (std::strcmp(mode, "crash") == 0) {
        recomp_stop_configure("completed", nullptr);
#ifdef _WIN32
        RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, 0, 0, nullptr);
#else
        std::raise(SIGFPE);
#endif
    }
    return 65;
}
