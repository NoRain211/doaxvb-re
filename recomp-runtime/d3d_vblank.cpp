#include "d3d_vblank.h"
#include "d3d_tuning.h"
#include "game_pacing.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point last_vblank;
#ifdef _WIN32
thread_local std::unique_ptr<void, decltype(&CloseHandle)>
    high_resolution_timer(nullptr, &CloseHandle);
#endif
// The rate override is a timing experiment, not a gameplay-speed correction.
std::chrono::nanoseconds interval(1000000000 / 60);
Clock::time_point probe_start;
unsigned probe_waits;
bool probe_enabled;
bool keep_debt;
std::chrono::nanoseconds waited{0};
constexpr auto spin_window = std::chrono::microseconds(500);
std::atomic<unsigned> pass_counts[3];
std::atomic<uint32_t> frame_cost_ns{0};
std::atomic<unsigned> display_rate{60};

void sleepUntil(Clock::time_point deadline)
{
#ifdef _WIN32
    if (high_resolution_timer == nullptr) {
        high_resolution_timer.reset(CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE | SYNCHRONIZE));
    }
    if (high_resolution_timer == nullptr) {
        std::this_thread::sleep_until(deadline);
        return;
    }

    const auto spin_deadline = deadline - spin_window;
    const auto timer_wait = spin_deadline - Clock::now();
    if (timer_wait > Clock::duration::zero()) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(
            timer_wait).count() / 100;
        if (due.QuadPart < 0 &&
            SetWaitableTimer(high_resolution_timer.get(), &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(high_resolution_timer.get(), INFINITE);
        } else {
            std::this_thread::sleep_until(deadline);
            return;
        }
    }
    while (Clock::now() < deadline) YieldProcessor();
#else
    const auto spin_deadline = deadline - spin_window;
    if (spin_deadline > Clock::now()) {
        std::this_thread::sleep_until(spin_deadline);
    }
    while (Clock::now() < deadline) {
#if defined(__aarch64__)
        __asm__ __volatile__("yield");
#else
        std::this_thread::yield();
#endif
    }
#endif
}

#ifdef _WIN32
// A key press this frame, only while one of this process's windows has focus.
bool tapped(int key)
{
    static bool down[256];
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    const bool now = process == GetCurrentProcessId() && (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool tap = now && !down[key];
    down[key] = now;
    return tap;
}

/* F5/F6 RTGI strength -/+, F7 split view, F8 RTGI on/off, F9 pause,
   F10 pause or step one frame, F11 cycle the G-buffer debug view.
   Returns true when F10 asked for a step. */
bool pollHotkeys()
{
    auto &t = recomp_tuning;
    const bool keys[] = {tapped(VK_F5), tapped(VK_F6), tapped(VK_F7),
        tapped(VK_F8), tapped(VK_F9), tapped(VK_F10), tapped(VK_F11)};
    const float strength = t.rtgi_strength.load();
    if (keys[0]) t.rtgi_strength = (std::max)(strength - 0.25f, 0.0f);
    if (keys[1]) t.rtgi_strength = (std::min)(strength + 0.25f, 4.0f);
    if (keys[2]) t.rtgi_split = !t.rtgi_split;
    if (keys[3]) t.rtgi_off = !t.rtgi_off;
    if (keys[4]) t.paused = !t.paused;
    const bool step = keys[5] && t.paused;
    if (keys[5]) t.paused = true;
    if (keys[6]) ++t.gbuffer_cycles;
    bool any = false;
    for (bool key : keys) any |= key;
    if (any) {
        wchar_t title[128];
        std::swprintf(title, 128, L"DOAXBV Recomp - RTGI %ls strength %.2f%ls%ls",
            t.rtgi_off ? L"off" : L"on", t.rtgi_strength.load(),
            t.rtgi_split ? L" split" : L"", t.paused ? L" - PAUSED (F9 resume, F10 step)" : L"");
        // The render worker owns the window; never block on it.
        SendMessageTimeoutW(GetForegroundWindow(), WM_SETTEXT, 0,
            reinterpret_cast<LPARAM>(title), SMTO_ABORTIFHUNG, 100, nullptr);
    }
    return step;
}
#else
// The tuning hotkeys read Win32 key state and retitle the D3D11 window.
bool pollHotkeys() { return false; }
#endif
}

void recomp_d3d_vblank_reset(void)
{
    const char *rate = std::getenv("RECOMP_GUEST_VBLANK_HZ");
    if (rate == nullptr) rate = std::getenv("RECOMP_GAME_HZ");
    char *end = nullptr;
    const unsigned long requested = rate != nullptr ? std::strtoul(rate, &end, 10) : 0;
    probe_enabled = end != rate && end != nullptr && *end == '\0' &&
        requested >= 60 && requested <= 360;
    const unsigned hz = probe_enabled ? static_cast<unsigned>(requested) : 60;
    keep_debt = std::getenv("RECOMP_GAME_HZ") != nullptr && hz > 60;
    interval = std::chrono::nanoseconds(1000000000 / hz);
    last_vblank = Clock::now();
    probe_start = last_vblank;
    probe_waits = 0;
    if (probe_enabled) {
        std::fprintf(stderr, "recomp vblank probe: hz=%u; elapsed starts at reset\n", hz);
    } else if (rate != nullptr) {
        std::fprintf(stderr, "recomp vblank probe: expected 60 to 360; using 60 Hz\n");
    }
}

uint64_t recomp_d3d_vblank_waited_ns(void)
{
    return static_cast<uint64_t>(waited.count());
}

void recomp_d3d_count_passes(unsigned ticks, unsigned displays, unsigned dropped)
{
    pass_counts[0] += ticks;
    pass_counts[1] += displays;
    pass_counts[2] += dropped;
}

void recomp_d3d_take_passes(unsigned counts[3])
{
    for (unsigned i = 0; i < 3; ++i) counts[i] = pass_counts[i].exchange(0);
}

void recomp_d3d_set_frame_cost_ns(uint32_t ns) { frame_cost_ns = ns; }
uint32_t recomp_d3d_frame_cost_ns(void) { return frame_cost_ns; }
void recomp_d3d_set_display_rate(unsigned hz) { display_rate = hz; }
unsigned recomp_d3d_display_rate(void) { return display_rate; }

void recomp_d3d_wait_vblank(void)
{
    if (probe_enabled && (++probe_waits == 1 || probe_waits % 300 == 0)) {
        const double elapsed = std::chrono::duration<double>(
            Clock::now() - probe_start).count();
        std::fprintf(stderr, "recomp vblank probe: wait=%u elapsed_s=%.6f\n",
            probe_waits, elapsed);
    }
    // Pausing holds the game here; the render worker keeps the window responsive.
    if (pollHotkeys()) recomp_tuning.paused = true;
    else while (recomp_tuning.paused) {
        sleepUntil(Clock::now() + interval);
        if (pollHotkeys()) break;
    }
    // Whole-game passes keep up to one refresh of debt: a tick pass that
    // overruns its refresh is made up by the display pass after it, so both
    // fit in one tick. Longer stalls (loads) are dropped, not replayed.
    const auto now = Clock::now();
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
        last_vblank - now).count();
    const int64_t delay = recomp_game_pacing_delay_ns(remaining, interval.count(), keep_debt);
    last_vblank = now + std::chrono::nanoseconds(std::max<int64_t>(delay, -interval.count()));
    sleepUntil(last_vblank);
    waited += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - now);
}

void recomp_d3d_sleep_until(long long steady_ns)
{
    sleepUntil(Clock::time_point(std::chrono::nanoseconds(steady_ns)));
}
