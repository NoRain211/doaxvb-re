#include "host_diagnostics.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace {
/* Unbuffered ucrt stderr issues one WriteFile per character when redirected,
   which stalled frames. Crash reporters and recomp_stop flush explicitly;
   the flusher bounds what a hang or hard kill can lose. */
void bufferStderr()
{
    setvbuf(stderr, nullptr, _IOFBF, 1 << 16);
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::fflush(stderr);
        }
    }).detach();
}
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
LONG CALLBACK reportUnhandledException(EXCEPTION_POINTERS *info)
{
    std::fprintf(stderr,
        "recomp host: event=host-crash kind=unhandled-exception"
        " exception_code=0x%08lx address=%p expectation_applied=false\n",
        static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
        info->ExceptionRecord->ExceptionAddress);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
}
#endif

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <initializer_list>

namespace {
alignas(16) unsigned char signal_stack[64 * 1024];

void reportFatalSignal(int signal, siginfo_t *, void *)
{
    /* Write the classification before unwinding: symbol lookup is best effort
       in a damaged process and must not hide the terminal event. */
    static const char event[] =
        "recomp host: event=host-crash kind=unhandled-signal expectation_applied=false\n";
    (void)write(STDERR_FILENO, event, sizeof event - 1u);
    void *frames[32];
    const int count = backtrace(frames, 32);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    _exit(128 + signal);
}

void installSignalDiagnostics()
{
    /* Load the unwinder before a signal can interrupt allocator initialization. */
    void *frame;
    backtrace(&frame, 1);
    stack_t stack{};
    stack.ss_sp = signal_stack;
    stack.ss_size = sizeof signal_stack;
    if (sigaltstack(&stack, nullptr) != 0) {
        std::perror("recomp host: sigaltstack");
    }
    struct sigaction action{};
    action.sa_sigaction = reportFatalSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        if (sigaction(signal, &action, nullptr) != 0) {
            std::perror("recomp host: sigaction");
        }
    }
}
}
#endif

#if defined(_MSC_VER) && defined(_DEBUG)

#include "runtime.h"

#include <crtdbg.h>
#include <dbghelp.h>
#include <rtcapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kGeneratedFramesReported = 8;

/* Runtime checks name only the offending variable, so the raising generated
   function is recovered from the host backtrace instead. */
void reportGeneratedFrames()
{
    void *frames[32];
    const USHORT captured =
        CaptureStackBackTrace(0u, 32u, frames, nullptr);
    const HANDLE process = GetCurrentProcess();
    unsigned char storage[sizeof(SYMBOL_INFO) + 256u] = {0};
    SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
    int reported = 0;

    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255u;
    for (USHORT i = 0u; i < captured && reported < kGeneratedFramesReported;
         ++i) {
        DWORD64 displacement = 0u;

        if (!SymFromAddr(
                process,
                reinterpret_cast<DWORD64>(frames[i]),
                &displacement,
                symbol)) {
            continue;
        }
        if (std::strncmp(symbol->Name, "report", 6) == 0 ||
            std::strncmp(symbol->Name, "_RTC", 4) == 0 ||
            std::strncmp(symbol->Name, "failwith", 8) == 0) {
            continue; /* this reporter's own frames */
        }
        std::fprintf(
            stderr,
            "%s%s+0x%llx",
            reported == 0 ? " frames=" : "<-",
            symbol->Name,
            static_cast<unsigned long long>(displacement));
        ++reported;
    }
    if (reported == 0) {
        std::fprintf(stderr, " frames=<unresolved>");
    }
}

const char *exceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    default: return "exception";
    }
}

LONG CALLBACK reportObservedException(EXCEPTION_POINTERS *info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;

    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_PRIV_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    std::fprintf(
        stderr,
        "recomp host: event=exception-observed terminal=unknown %s (0x%08lx) at 0x%p",
        exceptionName(code),
        static_cast<unsigned long>(code),
        info->ExceptionRecord->ExceptionAddress);
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2u) {
        std::fprintf(
            stderr,
            " %s 0x%p",
            info->ExceptionRecord->ExceptionInformation[0] == 8u
                ? "executing"
                : (info->ExceptionRecord->ExceptionInformation[0] != 0u
                       ? "writing"
                       : "reading"),
            reinterpret_cast<void *>(
                info->ExceptionRecord->ExceptionInformation[1]));
    }
    reportGeneratedFrames();
    std::fprintf(
        stderr,
        " esp=0x%08lx ebp=0x%08lx dispatch=0x%08lx\n",
        static_cast<unsigned long>(recomp_runtime.registers.esp),
        static_cast<unsigned long>(recomp_runtime.registers.ebp),
        static_cast<unsigned long>(recomp_last_dispatch_address));
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

int __cdecl reportRuntimeCheck(
    int errorType,
    const wchar_t *filename,
    int line,
    const wchar_t *module,
    const wchar_t *format,
    ...)
{
    wchar_t message[512];
    va_list arguments;

    va_start(arguments, format);
    _vsnwprintf_s(message, _TRUNCATE, format, arguments);
    va_end(arguments);

    std::fprintf(
        stderr,
        "recomp host: event=runtime-check terminal=false runtime check %d in %ls:%d module=%ls: %ls",
        errorType,
        filename != nullptr ? filename : L"<unknown>",
        line,
        module != nullptr ? module : L"<unknown>",
        message);
    std::fflush(stderr);
    reportGeneratedFrames();
    std::fprintf(
        stderr,
        " esp=0x%08lx ebp=0x%08lx dispatch=0x%08lx\n",
        static_cast<unsigned long>(recomp_runtime.registers.esp),
        static_cast<unsigned long>(recomp_runtime.registers.ebp),
        static_cast<unsigned long>(recomp_last_dispatch_address));

    /* Report without breaking so one guarded run collects every site. */
    return 0;
}

} // namespace

void recomp_install_host_diagnostics(void)
{
    bufferStderr();
#ifdef _WIN32
    SetUnhandledExceptionFilter(reportUnhandledException);
#endif
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    const int reports[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
    for (int report : reports) {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
    _RTC_SetErrorFuncW(reportRuntimeCheck);
    AddVectoredExceptionHandler(1u, reportObservedException);
}

#else

#include <cstdio>

void recomp_install_host_diagnostics(void)
{
    bufferStderr();
#ifdef _WIN32
    SetUnhandledExceptionFilter(reportUnhandledException);
#else
    installSignalDiagnostics();
#endif
}

#endif
