#include "game_scheduler.h"
#include "game_frame_replay.h"
#include "game_pacing.h"
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
        do { ++calls; } while (!recomp_game_sleep_step(&ticks, &fraction, hz / 60u));
        expect(calls * 60u == (unsigned)durations[i] * hz,
               "integer sleep duration changed in seconds");
        expect(ticks == 0 && fraction == 0, "completed sleep retained a fraction");
    }
    int32_t ticks = -1;
    unsigned fraction = 1;
    expect(recomp_game_sleep_step(&ticks, &fraction, hz / 60u) && ticks == 0 && fraction == 0,
           "nonpositive sleep did not resume immediately");
}

static void check_pacing(unsigned hz)
{
    const int64_t interval = INT64_C(1000000000) / hz;
    expect(recomp_game_pacing_delay_ns(-interval / 2, interval, false) ==
           interval - interval / 2, "on-time presentation delay changed");
    expect(recomp_game_pacing_delay_ns(-interval * 3, interval, false) == 0,
           "legacy late frame started catching up");
    expect(recomp_game_pacing_delay_ns(-interval * 3, interval, true) == -interval * 2,
           "fixed game clock discarded elapsed time");
    int64_t deadline = 0, now = 0;
    for (unsigned frame = 1; frame <= hz; ++frame) {
        now += frame == 10 ? interval * 5 : interval / 4;
        const int64_t wait = recomp_game_pacing_delay_ns(deadline - now, interval, true);
        deadline = now + wait;
        if (wait > 0) now += wait;
        expect(deadline == interval * frame, "game deadline accumulated processing drift");
    }
    expect(now == interval * hz, "game clock did not recover a transient late frame");
}

static void check_display_passes(void)
{
    const int64_t ms = 1000000;
    RecompGamePacing pacing;
    recomp_game_pacing_init(&pacing, 2);
    expect(recomp_game_pacing_display_passes(&pacing, 2, 0, 3 * ms, 2 * ms, 9 * ms) == 1,
           "kept a display pass whose render cost does not fit");
    unsigned passes = 1;
    for (unsigned i = 0; i < 600; ++i)
        passes = recomp_game_pacing_display_passes(&pacing, 2, 0, 3 * ms, 2 * ms, 15 * ms / 2);
    expect(passes == 1, "a pass returned inside the hysteresis band");
    recomp_game_pacing_init(&pacing, 2);
    for (unsigned i = 0; i < 1800; ++i)
        passes = recomp_game_pacing_display_passes(&pacing, 2, i % 20 == 0, 3 * ms, 2 * ms, 15 * ms / 2);
    expect(passes == 2, "a pass dropped inside the hysteresis band or on a rare miss");
    for (unsigned i = 0; i < 20; ++i)
        passes = recomp_game_pacing_display_passes(&pacing, 2, 1, 3 * ms, 2 * ms, 2 * ms);
    expect(passes == 2, "a short burst of misses dropped a pass");
    for (unsigned i = 0; i < 40; ++i)
        passes = recomp_game_pacing_display_passes(&pacing, 2, 1, 3 * ms, 2 * ms, 2 * ms);
    expect(passes == 1, "kept a pass that keeps missing its slot");
    /* The pass held over 5 s, so it returns after the two-second dwell. */
    for (unsigned i = 0; i < 119; ++i)
        passes = recomp_game_pacing_display_passes(&pacing, 2, 0, 3 * ms, 2 * ms, 2 * ms);
    expect(passes == 1, "a pass returned before two seconds of fitting");
    passes = recomp_game_pacing_display_passes(&pacing, 2, 0, 3 * ms, 2 * ms, 2 * ms);
    expect(passes == 2, "a fitting pass did not return");
    /* It drops again at once, so the next return waits twice as long. */
    for (unsigned i = 0; i < 60; ++i)
        passes = recomp_game_pacing_display_passes(&pacing, 2, 1, 3 * ms, 2 * ms, 2 * ms);
    for (unsigned i = 0; i < 239; ++i)
        passes = recomp_game_pacing_display_passes(&pacing, 2, 0, 3 * ms, 2 * ms, 2 * ms);
    expect(passes == 1, "a pass that failed again returned without a longer dwell");
    expect(recomp_game_pacing_display_passes(&pacing, 2, 0, 3 * ms, 2 * ms, 2 * ms) == 2,
           "a fitting pass did not return after the longer dwell");
    expect(recomp_game_pacing_display_passes(&pacing, 2, 0, 6 * ms, 6 * ms, 2 * ms) == 2,
           "game work alone dropped a pass that was not missing its slot");
    recomp_game_pacing_init(&pacing, 1);
    /* With no pass left, ticks present directly. */
    expect(recomp_game_pacing_display_passes(&pacing, 1, 1, 3 * ms, 2 * ms, 30 * ms) == 0,
           "kept a display pass that cannot fit");
    for (unsigned i = 0; i < 240; ++i)
        passes = recomp_game_pacing_display_passes(&pacing, 1, 0, 8 * ms, 8 * ms, 3 * ms);
    expect(passes == 1, "a stale pass cost kept the first pass from returning");
}

int recomp_game_scheduler_test(void)
{
    passed = 1;
    {
        /* A held present must redraw the tick's UI even after the guest
           reuses its dynamic vertex/index buffers. */
        uint8_t vertices[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        uint16_t indices[3] = {0, 1, 0};
        RecompD3dPresenterCommand command = {.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        RecompD3dPresenterDrawCommand *draw = &command.data.draw;
        draw->vertex_bytes = vertices; draw->vertex_count = 2; draw->vertex_stride = 4;
        draw->index_bytes = indices; draw->index_count = 3;
        RecompD3dPresenterCommand clear = {.type = RECOMP_D3D_PRESENTER_COMMAND_CLEAR};
        clear.data.clear.clear_depth = true;
        clear.data.clear.z = 1.0f;
        RecompGameFrameReplay replay = {0};
        /* The owner's depth clear sits between its draws and must replay in place. */
        for (unsigned i = 0; i < 70; ++i)
            expect(recomp_game_frame_replay_record(&replay, i == 35 ? &clear : &command),
                   "frame replay record failed");
        memset(vertices, 0, sizeof vertices);
        indices[1] = 9;
        const RecompD3dPresenterDrawCommand *last = &replay.commands[69].data.draw;
        expect(replay.commands[35].type == RECOMP_D3D_PRESENTER_COMMAND_CLEAR &&
               replay.commands[35].data.clear.clear_depth && replay.commands[35].data.clear.z == 1.0f &&
               replay.commands[34].type == RECOMP_D3D_PRESENTER_COMMAND_DRAW,
               "frame replay lost the owner's clear or its order");
        expect(replay.count == 70 && last->vertex_bytes != vertices &&
               ((const uint8_t *)last->vertex_bytes)[7] == 8 &&
               ((const uint16_t *)last->index_bytes)[1] == 1,
               "frame replay did not own its draw buffers");
        recomp_game_frame_replay_clear(&replay);
        expect(replay.count == 0, "frame replay clear kept draws");
    }
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
    check_pacing(60);
    check_pacing(120);
    check_pacing(240);
    check_display_passes();
    check_durations(60);
    check_durations(120);
    check_durations(240);
    recomp_runtime_init(regions, 2, NULL, 0, functions, 3);
    recomp_runtime.registers.esp = 0x010000d0u;
    *recomp_memory_u32(0x010000d4u) = 0x01000080u;
    recomp_game_test_clock_begin(false);
    expect(recomp_game_scheduler_lookup_manual(0x80000080u) == NULL,
           "normal system time was intercepted");
    recomp_game_test_clock_begin(true);
    RecompFunction startup_time = recomp_lookup_manual(0x80000080u);
    expect(startup_time != NULL, "startup clock fixture was not dispatched");
    if (startup_time != NULL) {
        startup_time();
        expect(*recomp_memory_u64(0x01000080u) == UINT64_C(125911584000000000) &&
               recomp_runtime.registers.esp == 0x010000d8u,
               "startup clock value or callee cleanup differs");
    }
    expect(recomp_game_scheduler_lookup_manual(0x80000080u) == NULL,
           "clock fixture changed later system-time reads");
    for (unsigned hz = 60; hz <= 240; hz *= 2) {
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
            *recomp_memory_u32(slot + 12u) = i == 1 ? 0x000a37f0u : 0;
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
        expect(resumed[0] == 2 && resumed[1] == 1,
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
        recomp_game_pass_begin(false, hz / 60u, 0);
        recomp_game_scheduler_tick();
        expect(resumed[0] == 2 && *recomp_memory_u32(0x005f2c3cu) == 1,
               "display pass resumed or aged a sleeping fiber");
        recomp_game_pass_begin(true, 1, 0);
    }
    recomp_runtime_init(NULL, 0, NULL, 0, NULL, 0);
    return passed;
}
