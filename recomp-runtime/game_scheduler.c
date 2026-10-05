#include "game_scheduler.h"
#include "game_input.h"
#include "game_scene.h"
#include "xapi_time_adapter.h"
#include "kernel_abi.h"
#include "d3d_draw_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    FIBER_COUNT = 17,
    FIBER_SLOTS = 0x005f2c38u,
    CURRENT_SLOT = 0x005f2e5cu,
    TIMESTAMP_BANK = 0x005f2e60u,
    FRAME_TIMESTAMP = 0x005f2e68u,
    SLOT_SIZE = 32,
};

static unsigned game_hz = 60;
static unsigned sleep_subticks[FIBER_COUNT];
static bool startup_clock_pending;
static bool trace_scheduler;
static unsigned scheduler_pass;
static bool trace_timing;
static uint64_t fiber_time[17], fiber_peak[17];
static unsigned fiber_calls[17];
static uint64_t phase_time[6];

static uint64_t timing_now(void)
{
    return trace_timing ? recomp_xapi_performance_counter() : 0;
}

unsigned recomp_game_requested_hz(void)
{
    const char *rate = getenv("RECOMP_GAME_HZ");
    if (rate == NULL) return 0;
    if (strcmp(rate, "60") == 0) return 60;
    if (strcmp(rate, "120") == 0) return 120;
    if (strcmp(rate, "240") == 0) return 240;
    return 0;
}

bool recomp_game_timing_enabled(void) { return trace_timing; }

unsigned recomp_game_scheduler_hz(void) { return game_hz; }

#ifdef RECOMP_FULL_PROGRAM
void sub_000BB290(void);
#endif

void recomp_game_test_clock_begin(bool enabled)
{
    startup_clock_pending = enabled;
}

static void startup_clock(void)
{
    /* The first system-time read inside game initialization seeds the CRT RNG.
       Fix only that read for paired captures; later clocks remain live. */
    const uint32_t output = kernel_arg(1u);
    startup_clock_pending = false;
    if (output != 0) {
        *recomp_memory_u64(output) = (UINT64_C(11644473600) + 946684800) * 10000000;
    }
    fprintf(stderr, "recomp game test clock: startup Unix seconds=946684800\n");
    kernel_return(1u, 0);
}

static bool test_clock_enabled(void)
{
    const char *enabled = getenv("RECOMP_GAME_TEST_CLOCK");
    return enabled != NULL && strcmp(enabled, "1") == 0;
}

#ifdef RECOMP_FULL_PROGRAM
static void generated_game_entry(void)
{
    recomp_game_test_clock_begin(test_clock_enabled());
    sub_000BB290();
}
#endif

bool recomp_game_sleep_step(int32_t *ticks, unsigned *subtick, unsigned steps)
{
    if (*ticks <= 0) {
        *ticks = 0;
        *subtick = 0;
        return true;
    }
    if (++*subtick < steps) {
        return false;
    }
    *subtick = 0;
    --*ticks;
    return *ticks == 0;
}

void recomp_game_scheduler_reset(unsigned hz)
{
    game_hz = hz;
    trace_scheduler = getenv("RECOMP_GAME_SCHEDULER_TRACE") != NULL;
    scheduler_pass = 0;
    trace_timing = getenv("RECOMP_GAME_TIMING_TRACE") != NULL;
    memset(fiber_time, 0, sizeof fiber_time);
    memset(fiber_peak, 0, sizeof fiber_peak);
    memset(fiber_calls, 0, sizeof fiber_calls);
    memset(phase_time, 0, sizeof phase_time);
    memset(sleep_subticks, 0, sizeof sleep_subticks);
}

static void push(uint32_t value)
{
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = value;
}

static void call(uint32_t address)
{
    const uint32_t saved_esp = recomp_runtime.registers.esp;
    push(0);
    recomp_dispatch_indirect_site(address, saved_esp, __FILE__, __LINE__);
}

static void call_one(uint32_t address, uint32_t argument)
{
    push(argument);
    call(address);
}

void recomp_game_scheduler_tick(void)
{
    ++scheduler_pass;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    *recomp_memory_u32(CURRENT_SLOT) = 0;
    while ((int32_t)*recomp_memory_u32(CURRENT_SLOT) < FIBER_COUNT) {
        const uint32_t index = *recomp_memory_u32(CURRENT_SLOT);
        const uint32_t slot = FIBER_SLOTS + index * SLOT_SIZE;
        const uint32_t bank = *recomp_memory_u32(TIMESTAMP_BANK);
        recomp_runtime.registers.esi = slot;
        call_one(0x001830cfu, slot + 16u + bank * 8u);
        const uint32_t flags = *recomp_memory_u32(slot);
        if ((flags & 1u) != 0 && (flags & 0x20u) == 0) {
            int32_t ticks = (int32_t)*recomp_memory_u32(slot + 4u);
            const uint32_t callback = *recomp_memory_u32(slot + 12u);
            const bool frame_owner = recomp_game_scene_lookup_manual(callback) != NULL ||
                callback == 0x0002eec0u || callback == 0x0008a970u;
            const bool resume = recomp_game_sleep_step(
                &ticks, &sleep_subticks[index], frame_owner ? 1u : game_hz / 60u);
            *recomp_memory_u32(slot + 4u) = (uint32_t)ticks;
            if (resume) {
                const uint32_t draws_before = recomp_d3d_draw_adapter_submitted();
                const uint64_t started = timing_now();
                call_one(0x0018305au, *recomp_memory_u32(slot + 8u));
                if (trace_timing) {
                    const uint64_t elapsed = timing_now() - started;
                    fiber_time[index] += elapsed;
                    if (elapsed > fiber_peak[index]) fiber_peak[index] = elapsed;
                    ++fiber_calls[index];
                }
                if (trace_scheduler && (scheduler_pass <= 70 || scheduler_pass % 300 == 0)) {
                    fprintf(stderr, "recomp game fiber: pass=%u slot=%u callback=%08x draws=%u scene_age=%u\n",
                        scheduler_pass, index, *recomp_memory_u32(slot + 12u),
                        recomp_d3d_draw_adapter_submitted() - draws_before,
                        *recomp_memory_u32(0x004d967cu));
                }
                if ((*recomp_memory_u32(slot) & 1u) == 0) {
                    const uint32_t handle = *recomp_memory_u32(slot + 8u);
                    *recomp_memory_u32(slot) = 0;
                    if (handle != 0) {
                        call_one(0x00183047u, handle);
                        *recomp_memory_u32(slot + 8u) = 0;
                    }
                }
            }
        } else if ((flags & 1u) == 0) {
            sleep_subticks[index] = 0;
        }
        ++*recomp_memory_u32(CURRENT_SLOT);
    }
    recomp_runtime.registers.esi = saved_esi;
    call_one(0x001830cfu, FRAME_TIMESTAMP +
             *recomp_memory_u32(TIMESTAMP_BANK) * 8u);
    *recomp_memory_u32(TIMESTAMP_BANK) ^= 1u;
    *recomp_memory_u32(CURRENT_SLOT) = UINT32_MAX;
}

static void scheduler_adapter(void)
{
    recomp_game_scheduler_tick();
    recomp_runtime.registers.esp += 4u;
}

/* This replaces the complete main-loop owner, including its movie branch.
   Generated neighbours retain their guest calling conventions and storage. */
static void game_loop(void)
{
    push(*recomp_ebp_register());
    *recomp_ebp_register() = recomp_runtime.registers.esp;
    recomp_runtime.registers.esp &= ~7u;
    recomp_runtime.registers.esp -= 20u;
    push(recomp_runtime.registers.esi);
    const uint32_t scratch = recomp_runtime.registers.esp;

    for (;;) {
        uint64_t timed = timing_now();
        call(0x000b57d0u); /* Poll controllers. */
        call(0x000b5870u); /* Derive edges and repeats. */
        call(0x000b5aa0u); /* Service feedback. */
        phase_time[0] += timing_now() - timed;
        timed = timing_now();
        call(0x001f90e0u);
        if (*recomp_memory_u32(0x005f2b98u) != 0) {
            call(0x001e38f0u);
            call_one(0x001e4830u, scratch + 8u);
            if (*recomp_memory_u32(scratch + 8u) == 2) {
                call(0x001e38f0u);
                call_one(0x001e4830u, scratch + 16u);
            }
        }
        push(0);
        push(0x3f800000u);
        push(*recomp_memory_u32(0x00a23bacu));
        push(0xf3u);
        push(0);
        push(0);
        call(0x001e72d0u);
        call_one(0x0017f2e0u, 0);
        recomp_runtime.registers.esp += 4u;
        *recomp_memory_u32(0x00a23bacu) = 0;
        call(0x00179b30u);
        phase_time[1] += timing_now() - timed;
        timed = timing_now();
        call(0x000b5680u);
        phase_time[2] += timing_now() - timed;
        timed = timing_now();
        call(0x000bab70u);
        phase_time[3] += timing_now() - timed;
        timed = timing_now();
        call(0x00179a90u);
        phase_time[4] += timing_now() - timed;
        timed = timing_now();
        if (*recomp_memory_u32(0x005f2b98u) != 0) {
            call(0x0018a160u);
            if (*recomp_memory_u32(0x00b3c34cu) != 0 &&
                (int32_t)*recomp_memory_u32(0x005f2b94u) < 4) {
                call(0x0018a560u);
            }
            push(0x005f2ba0u);
            push(*recomp_memory_u32(0x005f2b18u));
            call(0x00198190u);
            recomp_runtime.registers.esp += 8u;
            if (*recomp_memory_u32(0x005f2ba0u) != 0) {
                recomp_runtime.registers.esi = 0x005f2ba0u;
                call(0x000b4da0u);
                call_one(0x00197ec0u, *recomp_memory_u32(0x005f2b18u));
                recomp_runtime.registers.esp += 4u;
            }
        }
        phase_time[5] += timing_now() - timed;
        if (trace_timing && scheduler_pass % 300 == 0) {
            fprintf(stderr, "recomp game timing: pass=%u phase_ms=", scheduler_pass);
            for (unsigned i = 0; i < 6; ++i) fprintf(stderr, "%s%.3f", i ? "," : "", phase_time[i] / 1e6);
            fprintf(stderr, "\n");
            for (unsigned i = 0; i < 17; ++i) if (fiber_calls[i]) {
                fprintf(stderr, "recomp fiber timing: pass=%u slot=%u callback=%08x calls=%u total_ms=%.3f max_ms=%.3f\n",
                    scheduler_pass, i, *recomp_memory_u32(FIBER_SLOTS + i * SLOT_SIZE + 12u),
                    fiber_calls[i], fiber_time[i] / 1e6, fiber_peak[i] / 1e6);
            }
            memset(phase_time, 0, sizeof phase_time);
            memset(fiber_time, 0, sizeof fiber_time);
            memset(fiber_peak, 0, sizeof fiber_peak);
            memset(fiber_calls, 0, sizeof fiber_calls);
        }
    }
}

static void game_entry(void)
{
    recomp_game_test_clock_begin(test_clock_enabled());
    const unsigned hz = recomp_game_requested_hz();
    recomp_game_scheduler_reset(hz != 0 ? hz : 60);
    fprintf(stderr, "recomp game scheduler: hand source, hz=%u\n", game_hz);
    call(0x000bb2a0u);
    call(0x000bb310u);
}

/* Game-owned entry/loop/scheduler. The loop retains the existing generated
   D3D field-status/vblank and movie-service seams. Wholesale library replacement
   remains open; see docs/wayfinder/research/high-frame-rate-whole-game.md. */
RecompFunction recomp_game_scheduler_lookup_manual(uint32_t address)
{
    if (address == 0x80000080u && startup_clock_pending) {
        return startup_clock;
    }
    if (address != 0x000bb290u && address != 0x000bb310u &&
        address != 0x000b5680u && address != 0x000b5870u) {
        return NULL;
    }
    const char *enabled = getenv("RECOMP_GAME_SCHEDULER");
    if ((enabled == NULL || strcmp(enabled, "1") != 0) &&
        recomp_game_requested_hz() == 0) {
#ifdef RECOMP_FULL_PROGRAM
        if (address == 0x000bb290u && test_clock_enabled()) {
            return generated_game_entry;
        }
#endif
        return NULL;
    }
    switch (address) {
    case 0x000bb290u: return game_entry;
    case 0x000bb310u: return game_loop;
    case 0x000b5680u: return scheduler_adapter;
    case 0x000b5870u: return recomp_game_input_update;
    default: return NULL;
    }
}
