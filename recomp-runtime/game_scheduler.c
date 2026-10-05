#include "game_scheduler.h"
#include "game_frame_replay.h"
#include "game_input.h"
#include "game_scene.h"
#include "xapi_time_adapter.h"
#include "kernel_abi.h"
#include "d3d_draw_adapter.h"
#include "d3d_frame_adapter.h"
#include "stop_report.h"

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
static bool trace_rng;
static unsigned scheduler_pass;
static bool trace_timing;
static uint64_t fiber_time[17], fiber_peak[17];
static unsigned fiber_calls[17];
static uint64_t phase_time[6];
static const char *state_capture_path;
static uint32_t state_capture_at;
static bool main_present_pending;
/* The pass that polled input; the others hold the authored tick. */
static bool authored_pass = true;

bool recomp_game_authored_pass(void) { return authored_pass; }
static RecompGameFrameReplay frame_owner_draws;
/* Per-port records 0x5F3088 (4 x 44 bytes) and the aggregate at 0x5F3138.
   Polling, and so every press/release/repeat edge, happens on the authored
   pass. A fiber that sleeps whole ticks may resume on another pass of the
   tick; it receives that tick's edges once, as it would at 60 Hz. */
enum {
    INPUT_RECORDS = 0x005f3088u, INPUT_RECORD_BYTES = 5u * 44u,
    /* The scene fiber's per-port copy (0x000A5030), refreshed every pass. */
    SCENE_INPUT = 0x004d9540u, SCENE_INPUT_BYTES = 4u * 44u,
};
static uint8_t authored_input[INPUT_RECORD_BYTES];
static uint8_t pass_input[INPUT_RECORD_BYTES];
static uint8_t authored_scene_input[SCENE_INPUT_BYTES];
static uint8_t pass_scene_input[SCENE_INPUT_BYTES];
static bool tick_scope_active, tick_scope_input;
static float pass_step;
enum { STEP_SCALAR = 0x004d6fc4u };

/* Once-per-tick work runs with the authored step: every-pass owners see
   the step divided by the substep count, written each pass by the scene
   fiber at STEP_SCALAR. Off the polling pass, it also sees the tick's input. */
bool recomp_game_tick_scope_begin(void)
{
    if (tick_scope_active) return false;
    tick_scope_active = true;
    const unsigned steps = game_hz / 60u;
    if (steps > 1u) {
        recomp_guest_load(&pass_step, STEP_SCALAR, sizeof pass_step);
        const float step = pass_step * (float)steps;
        recomp_guest_store(STEP_SCALAR, &step, sizeof step);
    }
    tick_scope_input = !authored_pass;
    if (tick_scope_input) {
        recomp_guest_load(pass_input, INPUT_RECORDS, sizeof pass_input);
        recomp_guest_store(INPUT_RECORDS, authored_input, sizeof authored_input);
        recomp_guest_load(pass_scene_input, SCENE_INPUT, sizeof pass_scene_input);
        recomp_guest_store(SCENE_INPUT, authored_scene_input, sizeof authored_scene_input);
    }
    return true;
}

void recomp_game_tick_scope_end(bool begun)
{
    if (!begun) return;
    if (game_hz / 60u > 1u) recomp_guest_store(STEP_SCALAR, &pass_step, sizeof pass_step);
    if (tick_scope_input) {
        recomp_guest_store(INPUT_RECORDS, pass_input, sizeof pass_input);
        recomp_guest_store(SCENE_INPUT, pass_scene_input, sizeof pass_scene_input);
    }
    tick_scope_active = false;
}

void recomp_game_tick_input_scene_copied(void)
{
    if (authored_pass)
        recomp_guest_load(authored_scene_input, SCENE_INPUT, sizeof authored_scene_input);
}

static void record_frame_owner_draw(const struct RecompD3dPresenterDrawCommand *draw)
{
    if (!recomp_game_frame_replay_record(&frame_owner_draws, draw))
        recomp_stop(1, "game:frame-replay-allocation");
}

static void replay_frame_owner_draws(void)
{
    RecompD3dPresenterCommand command = {.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW};
    /* As in the draw adapter, a rejected draw (including after close) is a
       decline, not a runtime failure. */
    for (unsigned i = 0; i < frame_owner_draws.count; ++i) {
        command.data.draw = frame_owner_draws.draws[i];
        if (recomp_d3d_presenter_submit(recomp_d3d_frame_adapter_presenter(), &command) !=
            RECOMP_D3D_PRESENTER_OK) break;
    }
}

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
    trace_rng = getenv("RECOMP_GAME_RNG_TRACE") != NULL;
    scheduler_pass = 0;
    trace_timing = getenv("RECOMP_GAME_TIMING_TRACE") != NULL;
    memset(fiber_time, 0, sizeof fiber_time);
    memset(fiber_peak, 0, sizeof fiber_peak);
    memset(fiber_calls, 0, sizeof fiber_calls);
    memset(phase_time, 0, sizeof phase_time);
    state_capture_path = getenv("RECOMP_GAME_STATE_CAPTURE");
    const char *capture_at = getenv("RECOMP_GAME_STATE_AT");
    state_capture_at = state_capture_path && capture_at ? (uint32_t)strtoul(capture_at, NULL, 10) : 0;
    main_present_pending = false;
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

uint32_t recomp_game_rng_address(void)
{
    const uint32_t thread = *recomp_memory_u32(0x28u);
    uint32_t pointer = 0x00eeb28cu;
    if (*recomp_memory_u32(thread + 0x28u)) {
        const uint32_t tls = *recomp_memory_u32(4);
        const uint32_t index = *recomp_memory_u32(0x003b5258u);
        pointer = *recomp_memory_u32(tls + index * 4) + 0x1c;
    }
    const uint32_t data = *recomp_memory_u32(pointer);
    return data ? data + 0x14 : 0;
}

uint32_t recomp_game_debug_rng_state(void)
{
    if (!trace_rng) return 0;
    const uint32_t address = recomp_game_rng_address();
    return address ? *recomp_memory_u32(address) : 0;
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
            /* The unconverted UI frame owner counts one per tick on the
               polling pass, where input edges are visible; held passes
               resubmit its draws. Hand owners use an explicit zero yield;
               their generated initialization sleeps retain authored time. */
            const bool frame_owner = callback == 0x0008a970u;
            const bool held_owner = frame_owner && game_hz > 60u;
            const bool counted = ticks > 0;
            if (held_owner && authored_pass) recomp_game_frame_replay_clear(&frame_owner_draws);
            if (held_owner && !authored_pass) replay_frame_owner_draws();
            const bool resume = (!held_owner || authored_pass) && recomp_game_sleep_step(
                &ticks, &sleep_subticks[index], frame_owner ? 1u : game_hz / 60u);
            *recomp_memory_u32(slot + 4u) = (uint32_t)ticks;
            if (resume) {
                const uint32_t draws_before = recomp_d3d_draw_adapter_submitted();
                const uint32_t rng_before = trace_rng ? recomp_game_debug_rng_state() : 0;
                const uint64_t started = timing_now();
                if (held_owner) recomp_d3d_draw_adapter_observe(record_frame_owner_draw);
                const bool tick_scope = counted && recomp_game_tick_scope_begin();
                call_one(0x0018305au, *recomp_memory_u32(slot + 8u));
                /* Also closes a scope opened inside a fiber that a scene
                   transition replaced before it could restore the pass. */
                recomp_game_tick_scope_end(tick_scope || tick_scope_active);
                recomp_d3d_draw_adapter_observe(NULL);
                if (trace_rng && recomp_game_debug_rng_state() != rng_before) {
                    fprintf(stderr, "recomp rng: pass=%u swap=%u slot=%u callback=%08x before=%08x after=%08x\n",
                        scheduler_pass, recomp_d3d_frame_adapter_swap_counter(), index, callback,
                        rng_before, recomp_game_debug_rng_state());
                }
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

static void capture_game_state(void)
{
    if (trace_scheduler && (scheduler_pass <= 200 || scheduler_pass % 300 == 0)) {
        fprintf(stderr, "recomp game state trace: pass=%u swap=%u age=%u scene=%u scenario=%u subage=%u camera_yaw=%u target_yaw=%u\n",
            scheduler_pass, recomp_d3d_frame_adapter_swap_counter() + 1u,
            *recomp_memory_u32(0x004d967cu), *recomp_memory(0x005dece0u, 1),
            *recomp_memory_u32(0x004d994cu), *recomp_memory_u32(0x004d9950u),
            *recomp_memory_u32(0x0041a934u), *recomp_memory_u32(0x0041a96cu));
    }
    if (!state_capture_at || recomp_d3d_frame_adapter_swap_counter() + 1u != state_capture_at) return;
    static const uint32_t ranges[][2] = {
        {0x0041a7e0u, 0x4a40u}, {0x004d6fc0u, 0x2a40u},
        {0x009d4900u, 0xe00u}, {0x005dece0u, 0xa0u},
        {0x005e0680u, 0x1000u},
        {0x004d5600u, 0x100u},
        {0x00327c80u, 0x200u}, {0x005bb260u, 0x20u},
        {0x004256e0u, 0xa0020u},
        {0x00327400u, 0x880u}, {0x004c9700u, 0x2100u},
        {0x004d3650u, 0x3960u}, {0x005e5fe8u, 0x750u},
        {0x005bb960u, 0x1e0u},
        {0x005c3770u, 0x50u}, {0x00326fa0u, 8u},
    };
    FILE *output = fopen(state_capture_path, "wb");
    if (output != NULL) {
        for (unsigned i = 0; i < sizeof ranges / sizeof ranges[0]; ++i) {
            fwrite(ranges[i], sizeof ranges[i], 1, output);
            fwrite(recomp_memory(ranges[i][0], ranges[i][1]), ranges[i][1], 1, output);
        }
        for (unsigned group = 0; group < 2; ++group) {
            const uint32_t descriptor = *recomp_memory_u32(0x00326fa0u + group * 4);
            if (!descriptor) continue;
            const uint32_t header[] = {descriptor, 20};
            fwrite(header, sizeof header, 1, output);
            fwrite(recomp_memory(descriptor, 20), 20, 1, output);
            const uint32_t count = *recomp_memory_u16(descriptor + 16);
            const uint32_t records[] = {*recomp_memory_u32(descriptor), count * 64};
            if (records[0] && count < 4096) {
                fwrite(records, sizeof records, 1, output);
                fwrite(recomp_memory(records[0], records[1]), records[1], 1, output);
            }
        }
        fclose(output);
        fprintf(stderr, "recomp game state: swap=%u path=%s\n", state_capture_at, state_capture_path);
    }
    state_capture_at = 0;
}

/* Initialization and render-target changes also issue synchronous Swaps.
   Those remain authored-duration operations while the main loop produces
   one new frame per substep. Delivery stays in the native D3D adapter. */
static void game_swap(void)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const unsigned presents = main_present_pending ? 1u : game_hz / 60u;
    main_present_pending = false;
    for (unsigned i = 0; i < presents; ++i) {
        recomp_runtime.registers.esp = stack;
        recomp_d3d_swap_adapter();
    }
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
    unsigned poll_phase = 0;
    const unsigned steps = game_hz / 60u;

    for (;;) {
        uint64_t timed = timing_now();
        authored_pass = poll_phase == 0;
        if (poll_phase == 0) call(0x000b57d0u);
        poll_phase = (poll_phase + 1u) % steps;
        call(0x000b5870u); /* Derive edges and repeats. */
        if (authored_pass)
            recomp_guest_load(authored_input, INPUT_RECORDS, sizeof authored_input);
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
        capture_game_state();
        main_present_pending = true;
        call(0x00179a90u);
        main_present_pending = false;
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
    if (address == 0x001e8f30u && recomp_game_requested_hz() > 60u) {
        return game_swap;
    }
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
