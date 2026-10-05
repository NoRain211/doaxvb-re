#include "game_scheduler.h"
#include "program_manual.h"

#include <stdio.h>
#include <string.h>

static unsigned resumed[17];
static unsigned deleted;
static unsigned timestamps;
static uint32_t timestamp_addresses[36];
static int passed;

static void expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "game scheduler: %s\n", message);
        passed = 0;
    }
}

static void counter(void)
{
    uint32_t output = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    if (timestamps < 36) timestamp_addresses[timestamps] = output;
    ++timestamps;
    *recomp_memory_u64(output) = timestamps;
    recomp_runtime.registers.esp += 8u;
}

static void resume_fiber(void)
{
    uint32_t index = *recomp_memory_u32(0x005f2e5cu);
    uint32_t slot = 0x005f2c38u + index * 32u;
    expect(*recomp_memory_u32(recomp_runtime.registers.esp + 4u) == index + 1u,
           "resumed wrong handle or changed scheduler order");
    ++resumed[index];
    if (index == 2 || index == 3) {
        *recomp_memory_u32(slot) = 0x10;
        if (index == 3) *recomp_memory_u32(slot + 8u) = 0;
    } else {
        *recomp_memory_u32(slot + 4u) = 1;
    }
    recomp_runtime.registers.esp += 8u;
}

static void delete_fiber(void)
{
    expect(*recomp_memory_u32(recomp_runtime.registers.esp + 4u) == 3,
           "deleted an active, sleeping, or null fiber");
    ++deleted;
    recomp_runtime.registers.esp += 8u;
}

static void check_durations(unsigned hz)
{
    const int durations[] = {1, 2, 5, 20, 30, 3000, 5400};
    for (unsigned i = 0; i < sizeof durations / sizeof durations[0]; ++i) {
        int32_t ticks = durations[i];
        unsigned fraction = 0;
        unsigned calls = 0;
        do { ++calls; } while (!recomp_game_sleep_step(&ticks, &fraction, hz));
        expect(calls * 60u == (unsigned)durations[i] * hz,
               "integer sleep duration changed in seconds");
        expect(ticks == 0 && fraction == 0, "completed sleep retained a fraction");
    }
    int32_t ticks = -1;
    unsigned fraction = 1;
    expect(recomp_game_sleep_step(&ticks, &fraction, hz) && ticks == 0 && fraction == 0,
           "nonpositive sleep did not resume immediately");
}

int recomp_game_scheduler_test(void)
{
    static uint8_t slots[0x250];
    static uint8_t stack[256];
    const RecompMemoryRegion regions[] = {
        {0x005f2c38u, sizeof slots, slots},
        {0x01000000u, sizeof stack, stack},
    };
    const RecompFunctionEntry functions[] = {
        {0x001830cfu, counter},
        {0x0018305au, resume_fiber},
        {0x00183047u, delete_fiber},
    };
    passed = 1;
    check_durations(60);
    check_durations(120);
    for (unsigned hz = 60; hz <= 120; hz += 60) {
        memset(slots, 0, sizeof slots);
        memset(resumed, 0, sizeof resumed);
        timestamps = deleted = 0;
        recomp_runtime_init(regions, 2, NULL, 0, functions, 3);
        recomp_game_scheduler_reset(hz);
        recomp_runtime.registers.esp = 0x010000f0u;
        recomp_runtime.registers.esi = 0x12345678u;
        for (unsigned i = 0; i < 6; ++i) {
            uint32_t slot = 0x005f2c38u + i * 32u;
            *recomp_memory_u32(slot) = i == 4 ? 0x21 : i == 5 ? 0 : 1;
            *recomp_memory_u32(slot + 4u) = i == 1 ? 2 : i >= 4 ? 7 : 0;
            *recomp_memory_u32(slot + 8u) = i + 1;
        }
        recomp_game_scheduler_tick();
        expect(resumed[0] == 1 && resumed[1] == 0 && resumed[2] == 1 &&
               resumed[3] == 1 && resumed[4] == 0 && resumed[5] == 0,
               "initial ready/sleeping/paused/inactive selection differs");
        expect(deleted == 1 && *recomp_memory_u32(0x005f2c80u) == 0 &&
               *recomp_memory_u32(0x005f2c78u) == 0 &&
               *recomp_memory_u32(0x005f2c98u) == 0,
               "completed fiber cleanup differs");
        recomp_game_scheduler_tick();
        expect(resumed[0] == (hz == 60 ? 2u : 1u) &&
               resumed[1] == (hz == 60 ? 1u : 0u),
               "one-tick or two-tick sleep resumed at wrong time");
        expect(timestamps == 36 && timestamp_addresses[0] == 0x005f2c48u &&
               timestamp_addresses[17] == 0x005f2e68u &&
               timestamp_addresses[18] == 0x005f2c50u &&
               timestamp_addresses[35] == 0x005f2e70u,
               "timestamp ordering or alternating bank differs");
        expect(*recomp_memory_u32(0x005f2e5cu) == UINT32_MAX &&
               *recomp_memory_u32(0x005f2e60u) == 0 &&
               recomp_runtime.registers.esi == 0x12345678u &&
               recomp_runtime.registers.esp == 0x010000f0u,
               "scheduler leaked stack, callee-save register, index, or bank");
        expect(*recomp_memory_u32(0x005f2cbcu) == 7 &&
               *recomp_memory_u32(0x005f2cdcu) == 7,
               "paused/inactive sleep changed");
    }
    recomp_runtime_init(NULL, 0, NULL, 0, NULL, 0);
    return passed;
}
