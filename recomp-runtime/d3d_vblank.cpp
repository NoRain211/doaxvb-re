#include "d3d_vblank.h"
#include "game_pacing.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point last_vblank;
HANDLE high_resolution_timer;
// The rate override is a timing experiment, not a gameplay-speed correction.
std::chrono::nanoseconds interval(1000000000 / 60);
Clock::time_point probe_start;
unsigned probe_waits;
bool probe_enabled;
bool preserve_elapsed;
constexpr auto spin_window = std::chrono::microseconds(500);
}

void recomp_d3d_vblank_reset(void)
{
    const char *rate = std::getenv("RECOMP_GUEST_VBLANK_HZ");
    if (rate == nullptr) rate = std::getenv("RECOMP_GAME_HZ");
    probe_enabled = rate != nullptr &&
        (std::strcmp(rate, "60") == 0 || std::strcmp(rate, "120") == 0 ||
         std::strcmp(rate, "240") == 0);
    const unsigned hz = probe_enabled ? static_cast<unsigned>(std::atoi(rate)) : 60;
    const char *game_rate = std::getenv("RECOMP_GAME_HZ");
    preserve_elapsed = game_rate != nullptr &&
        (std::strcmp(game_rate, "120") == 0 || std::strcmp(game_rate, "240") == 0);
    interval = std::chrono::nanoseconds(1000000000 / hz);
    last_vblank = Clock::now();
    probe_start = last_vblank;
    probe_waits = 0;
    if (probe_enabled) {
        std::fprintf(stderr, "recomp vblank probe: hz=%u; elapsed starts at reset\n", hz);
    } else if (rate != nullptr) {
        std::fprintf(stderr, "recomp vblank probe: expected 60, 120 or 240; using 60 Hz\n");
    }
}

void recomp_d3d_wait_vblank(void)
{
    if (probe_enabled && (++probe_waits == 1 || probe_waits % 300 == 0)) {
        const double elapsed = std::chrono::duration<double>(
            Clock::now() - probe_start).count();
        std::fprintf(stderr, "recomp vblank probe: wait=%u elapsed_s=%.6f\n",
            probe_waits, elapsed);
    }
    // The opt-in fixed game clock keeps elapsed time through late frames.
    // The original presentation-only path continues to discard timing debt.
    const auto now = Clock::now();
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
        last_vblank - now).count();
    last_vblank = now + std::chrono::nanoseconds(recomp_game_pacing_delay_ns(
        remaining, interval.count(), preserve_elapsed));
    if (high_resolution_timer == nullptr) {
        high_resolution_timer = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
    }
    if (high_resolution_timer == nullptr) {
        std::this_thread::sleep_until(last_vblank);
        return;
    }

    const auto spin_deadline = last_vblank - spin_window;
    const auto timer_wait = spin_deadline - Clock::now();
    if (timer_wait > Clock::duration::zero()) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(
            timer_wait).count() / 100;
        if (due.QuadPart < 0 &&
            SetWaitableTimer(high_resolution_timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(high_resolution_timer, INFINITE);
        } else {
            std::this_thread::sleep_until(last_vblank);
            return;
        }
    }
    while (Clock::now() < last_vblank) YieldProcessor();
}
