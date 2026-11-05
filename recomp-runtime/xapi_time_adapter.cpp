#include "xapi_time_adapter.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

/* Adapted from doa2u-re's xapi_clock.cpp. All guest threads are fibers on
   the one game thread, so plain statics suffice. */
constexpr uint64_t frame_ns = UINT64_C(1000000000) / 60u;
/* Timing loops in the guest poll the counter. Each read moves time on by a
   small quantum so they make progress; the boundary discards it. */
constexpr uint64_t read_tick_ns = 1000u;
/* 2003-01-01 00:00 UTC as a FILETIME: the date netplay runners share. */
constexpr uint64_t frame_clock_epoch_filetime = UINT64_C(126858528000000000);
struct FrameClock {
    uint64_t base_ns;
    uint64_t offset_ns;
    uint64_t frame;
};
FrameClock frame_clock;
bool frame_clock_enabled;

struct FrameClockEnv {
    FrameClockEnv()
    {
        const char *value = std::getenv("RECOMP_FRAME_CLOCK");
        frame_clock_enabled = value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
    }
};
const FrameClockEnv frame_clock_env;

}

uint64_t recomp_xapi_host_ns(void)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t recomp_xapi_performance_counter(void)
{
    if (frame_clock_enabled) {
        frame_clock.offset_ns += read_tick_ns;
        /* Reads inside a frame never pass its end, so the next frame starts on the grid. */
        if (frame_clock.offset_ns > frame_ns) frame_clock.offset_ns = frame_ns;
        return frame_clock.base_ns + frame_clock.offset_ns;
    }
    return recomp_xapi_host_ns();
}

bool recomp_xapi_frame_clock_enabled(void) { return frame_clock_enabled; }

void recomp_xapi_frame_clock_set_enabled(bool enabled)
{
    recomp_xapi_frame_clock_reset();
    frame_clock_enabled = enabled;
}

void recomp_xapi_frame_clock_reset(void) { frame_clock = FrameClock{}; }

void recomp_xapi_frame_clock_advance(void)
{
    if (!frame_clock_enabled) return;
    frame_clock.base_ns += frame_ns;
    frame_clock.offset_ns = 0;
    ++frame_clock.frame;
}

size_t recomp_xapi_frame_clock_state_size(void) { return sizeof frame_clock; }
void recomp_xapi_frame_clock_state_save(void *state) { std::memcpy(state, &frame_clock, sizeof frame_clock); }
void recomp_xapi_frame_clock_state_restore(const void *state) { std::memcpy(&frame_clock, state, sizeof frame_clock); }

uint64_t recomp_xapi_system_filetime(void)
{
    /* In 100 ns units, so the date moves with the frame-locked counter. */
    if (frame_clock_enabled) return frame_clock_epoch_filetime + recomp_xapi_performance_counter() / 100u;
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) return 0u;
    return (static_cast<uint64_t>(now.tv_sec) + 11644473600ull) * 10000000ull +
        static_cast<uint64_t>(now.tv_nsec) / 100ull;
}

uint64_t recomp_xapi_guest_file_time(uint64_t host_filetime)
{
    return frame_clock_enabled ? frame_clock_epoch_filetime : host_filetime;
}

uint64_t recomp_xapi_performance_frequency(void)
{
    return UINT64_C(1000000000);
}

namespace {

void return_time_value(uint64_t value)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t destination = *recomp_memory_u32(entry_esp + 4u);
    std::memcpy(recomp_memory(destination, sizeof(value)), &value, sizeof(value));
    recomp_runtime.registers.eax = 1u;
    recomp_runtime.registers.esp = entry_esp + 8u;
}

void query_performance_counter(void)
{
    return_time_value(recomp_xapi_performance_counter());
}

void query_performance_frequency(void)
{
    return_time_value(recomp_xapi_performance_frequency());
}

}

RecompFunction recomp_xapi_time_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case 0x001830cfu:
        return query_performance_counter;
    case 0x001830e0u:
        return query_performance_frequency;
    default:
        return nullptr;
    }
}
