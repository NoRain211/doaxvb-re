#include "game_scheduler.h"
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
static unsigned sleep_half_ticks[FIBER_COUNT];
static bool startup_clock_pending;
static bool trace_scheduler;
static unsigned scheduler_pass;

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

bool recomp_game_sleep_step(int32_t *ticks, unsigned *half_tick, unsigned hz)
{
    if (*ticks <= 0) {
        *ticks = 0;
        *half_tick = 0;
        return true;
    }
    if (hz == 120 && ++*half_tick < 2) {
        return false;
    }
    *half_tick = 0;
    --*ticks;
    return *ticks == 0;
}

void recomp_game_scheduler_reset(unsigned hz)
{
    game_hz = hz;
    trace_scheduler = getenv("RECOMP_GAME_SCHEDULER_TRACE") != NULL;
    scheduler_pass = 0;
    memset(sleep_half_ticks, 0, sizeof sleep_half_ticks);
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
            const bool resume = recomp_game_sleep_step(
                &ticks, &sleep_half_ticks[index], game_hz);
            *recomp_memory_u32(slot + 4u) = (uint32_t)ticks;
            if (resume) {
                const uint32_t draws_before = recomp_d3d_draw_adapter_submitted();
                call_one(0x0018305au, *recomp_memory_u32(slot + 8u));
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
            sleep_half_ticks[index] = 0;
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
        call(0x000b57d0u); /* Poll controllers. */
        call(0x000b5870u); /* Derive edges and repeats. */
        call(0x000b5aa0u); /* Service feedback. */
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
        call(0x000b5680u);
        call(0x000bab70u);
        call(0x00179a90u);
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
    }
}

static void game_entry(void)
{
    recomp_game_test_clock_begin(test_clock_enabled());
    recomp_game_scheduler_reset(60);
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
    if (address != 0x000bb290u && address != 0x000bb310u && address != 0x000b5680u) {
        return NULL;
    }
    const char *enabled = getenv("RECOMP_GAME_SCHEDULER");
    if (enabled == NULL || strcmp(enabled, "1") != 0) {
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
    default: return NULL;
    }
}
