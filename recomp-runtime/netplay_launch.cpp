/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_launch.h"

#include "d3d_frame_adapter.h"
#include "free_camera_adapter.h"
#include "input_adapter.h"
#include "netplay_menu.h"
#include "netplay_profile.h"
#include "netplay_session.h"
#include "rollback.h"
#include "save_transaction.h"
#include "stop_report.h"
#include "synctest.h"
#include "xapi_time_adapter.h"
extern "C" {
#include "game_scheduler.h"
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

// Each takes a value, and a runner started again goes without them.
const char *const netplayFlags[] = {"--netplay-host", "--netplay-join", "--netplay-delay", "--netplay-room-socket",
    "--netplay-room", "--netplay-return", "--replay"};

RecompNetplayLaunch launch;
std::string replayPath;
std::string returnReason; // --netplay-return: why the room's session ended
std::string runnerPath;
std::vector<std::string> runnerArguments;

// Digits only, from min to max.
bool parseNumber(const char *text, std::uint32_t min, std::uint32_t max, std::uint32_t &value)
{
    std::uint64_t parsed = 0u;
    for (const char *c = text; *c != '\0'; ++c) {
        if (*c < '0' || *c > '9' || parsed > max) return false;
        parsed = parsed * 10u + static_cast<std::uint32_t>(*c - '0');
    }
    if (*text == '\0' || parsed < min || parsed > max) return false;
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

/* RECOMP_SYNCTEST: saves a frame at the loop top, runs it again and compares
   (docs/rollback-netcode.md). */
void synctestBoundary()
{
    static std::uint32_t frame;
    recomp_xapi_frame_clock_advance();
    const RecompSyncTestResult result = recomp_synctest_frame(++frame);
    if (result == RECOMP_SYNCTEST_MISMATCH) recomp_stop(3, "synctest:mismatch");
    if (!recomp_synctest_loop() &&
        (result == RECOMP_SYNCTEST_MATCH || result == RECOMP_SYNCTEST_SKIP)) {
        recomp_stop(0, result == RECOMP_SYNCTEST_MATCH ? "synctest:match" : "synctest:skip");
    }
}

// A running netplay session decides the guest's pads; before it starts, the usual sources do.
bool sessionSample(std::uint32_t port, RecompInputGamepad *gamepad)
{
    bool present = false;
    if (recomp_netplay_session_pad(port, gamepad, &present)) return present;
    return recomp_free_camera_sample(port, gamepad);
}

// Offline runs: the online menu sees each pad first (netplay_menu.cpp).
bool menuSample(std::uint32_t port, RecompInputGamepad *gamepad)
{
    const bool present = recomp_free_camera_sample(port, gamepad);
    if (present) recomp_netplay_menu_filter(port, gamepad);
    return present;
}

void menuFrame()
{
    recomp_netplay_menu_frame();
    std::vector<std::string> session;
    if (recomp_netplay_session_room_launch(session)) recomp_runner_relaunch(session);
}

#ifdef _WIN32
std::wstring commandLineArgument(const std::string &arg)
{
    const int size = MultiByteToWideChar(CP_ACP, 0, arg.c_str(), -1, nullptr, 0);
    std::wstring wide(size > 0 ? static_cast<std::size_t>(size - 1) : 0u, L'\0');
    if (size > 1) MultiByteToWideChar(CP_ACP, 0, arg.c_str(), -1, wide.data(), size);
    if (!wide.empty() && wide.find_first_of(L" \t\"") == std::wstring::npos) return wide;
    // CommandLineToArgvW's rules: backslashes double before a quote.
    std::wstring quoted = L"\"";
    std::size_t slashes = 0u;
    for (const wchar_t c : wide) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        quoted.append(c == L'"' ? slashes * 2u + 1u : slashes, L'\\');
        slashes = 0u;
        quoted += c;
    }
    quoted.append(slashes * 2u, L'\\');
    return quoted + L"\"";
}
#endif

} // namespace

bool recomp_netplay_launch_parse(int &argc, char **argv)
{
    runnerPath = argv[0];
    int kept = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (std::find(std::begin(netplayFlags), std::end(netplayFlags), arg) == std::end(netplayFlags)) {
            runnerArguments.push_back(arg);
            argv[kept++] = argv[i];
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "recomp runner: " << arg << " needs a value\n";
            return false;
        }
        const char *value = argv[++i];
        if (arg == "--netplay-host") {
            if (!parseNumber(value, 1u, 65535u, launch.hostPort)) {
                std::cerr << "recomp runner: --netplay-host needs a UDP port\n";
                return false;
            }
        } else if (arg == "--netplay-delay") {
            if (!parseNumber(value, 0u, RECOMP_ROLLBACK_WINDOW, launch.delay)) {
                std::cerr << "recomp runner: --netplay-delay is at most " << RECOMP_ROLLBACK_WINDOW << " frames\n";
                return false;
            }
        } else if (arg == "--netplay-join") {
            launch.join = value;
        } else if (arg == "--netplay-room-socket") {
            launch.roomSocket = value; // from a room's runner (recomp_netplay_session_room_launch)
        } else if (arg == "--netplay-room") {
            launch.room = value;
        } else if (arg == "--netplay-return") {
            returnReason = value;
        } else {
            replayPath = value;
        }
    }
    argc = kept;
    argv[argc] = nullptr;
    return true;
}

int recomp_netplay_launch_configure(const std::string &digest, const std::filesystem::path &disc)
{
    launch.digest = digest;
    launch.disc = disc;
    const char *testSession = std::getenv("RECOMP_ROLLBACK");
    if (testSession != nullptr && (*testSession == '\0' || std::strcmp(testSession, "0") == 0)) testSession = nullptr;
    const bool lobby = launch.hostPort != 0u || !launch.join.empty();
    if ((launch.hostPort != 0u) + !launch.join.empty() + !replayPath.empty() + (testSession != nullptr) > 1) {
        std::cerr << "recomp runner: --netplay-host, --netplay-join, --replay and RECOMP_ROLLBACK cannot be combined\n";
        return 64;
    }
    if (recomp_synctest_enabled() && (lobby || !replayPath.empty() || testSession != nullptr)) {
        std::cerr << "recomp runner: RECOMP_SYNCTEST cannot run with a netplay session or replay\n";
        return 2;
    }
    if (lobby && !recomp_netplay_session_connect(launch)) {
        // A room's runner goes back to the menu; one from the command line stops.
        if (!launch.roomSocket.empty())
            recomp_runner_relaunch(recomp_netplay_session_return_args(launch, "netplay:session-failed"));
        return 1;
    }
    if (!replayPath.empty() && !recomp_netplay_session_load_replay(replayPath, launch)) return 1;
    if (testSession != nullptr && !recomp_netplay_session_read_env(testSession)) return 2;
    return 0;
}

void recomp_netplay_launch_install(std::uint8_t *ram, std::size_t size, bool writeWatch)
{
    if (recomp_netplay_session_enabled()) {
        recomp_netplay_session_install({ram, size, writeWatch});
        recomp_input_adapter_set_source(sessionSample);
        std::atexit(recomp_netplay_session_exit);
    } else if (recomp_synctest_enabled()) {
        recomp_xapi_frame_clock_set_enabled(true);
        recomp_synctest_install_default_modules();
        recomp_synctest_set_guest_ram(ram, size, writeWatch);
        recomp_game_set_frame_boundary(synctestBoundary, true);
    } else {
        const std::filesystem::path storage = launch.disc / ".recomp-storage";
        recomp_netplay_menu_install(storage, launch.digest,
            [storage] { return recomp_netplay_own_profile_name(recomp_save_partition1(storage, nullptr)); });
        recomp_input_adapter_set_source(menuSample);
        recomp_d3d_swap_hook = menuFrame;
        if (!returnReason.empty() && !recomp_netplay_session_reopen_room(launch, returnReason))
            recomp_netplay_menu_reopen(returnReason);
    }
}

void recomp_runner_relaunch(const std::vector<std::string> &extra)
{
    std::vector<std::string> args{runnerPath};
    args.insert(args.end(), runnerArguments.begin(), runnerArguments.end());
    args.insert(args.end(), extra.begin(), extra.end());
    std::fflush(stderr);
    std::cout.flush();
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring line;
    for (const std::string &arg : args) line += (line.empty() ? L"" : L" ") + commandLineArgument(arg);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    // The new runner writes where this one did.
    const DWORD streams[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    HANDLE *handles[3] = {&startup.hStdInput, &startup.hStdOutput, &startup.hStdError};
    for (int i = 0; i < 3; ++i) {
        *handles[i] = GetStdHandle(streams[i]);
        if (*handles[i] != nullptr && *handles[i] != INVALID_HANDLE_VALUE)
            SetHandleInformation(*handles[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    PROCESS_INFORMATION process{};
    if (length == 0u || length == MAX_PATH ||
        !CreateProcessW(exe, line.data(), nullptr, nullptr, TRUE, 0u, nullptr, nullptr, &startup, &process))
        recomp_stop(4, "netplay:relaunch");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    recomp_stop(0, "netplay:relaunch");
#else
    std::vector<char *> argv;
    for (std::string &arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    execv(runnerPath.c_str(), argv.data());
    recomp_stop(4, "netplay:relaunch");
#endif
    std::abort(); // recomp_stop exits
}
