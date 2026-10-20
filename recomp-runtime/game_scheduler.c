#include "game_scheduler.h"
#include "game_frame_replay.h"
#include "game_input.h"
#include "game_rollback.h"
#include "game_display.h"
#include "game_scene.h"
#include "game_camera.h"
#include "xapi_time_adapter.h"
#include "kernel_abi.h"
#include "d3d_draw_adapter.h"
#include "d3d_frame_adapter.h"
#include "d3d_vblank.h"
#include "game_pacing.h"
#include "stop_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sched.h>
#include <time.h>
static void Sleep(unsigned ms)
{
    const struct timespec delay = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    nanosleep(&delay, NULL);
}
#define YieldProcessor() ((void)sched_yield())
#endif

enum {
    FIBER_COUNT = 17,
    FIBER_SLOTS = 0x005f2c38u,
    CURRENT_SLOT = 0x005f2e5cu,
    TIMESTAMP_BANK = 0x005f2e60u,
    FRAME_TIMESTAMP = 0x005f2e68u,
    SLOT_SIZE = 32,
};

static unsigned game_hz = 60;
static bool startup_clock_pending;
static bool trace_scheduler;
static bool trace_rng;
static unsigned scheduler_pass;
static bool trace_timing;
static uint64_t fiber_time[17], fiber_peak[17];
static unsigned fiber_calls[17];
/* Loop phases; 6 is tick pacing and 7 rollback, both also inside 0 or 5. */
static uint64_t phase_time[8];
static const char *state_capture_path;
static uint32_t state_capture_at;
static bool main_present_pending;
/* Authoritative ticks never consume interpolated state. Display passes draw
   completed tick pairs, then roll back before any further pass. */
static bool authored_pass = true;
static unsigned pass_steps = 1, pass_substep;
static uint32_t game_pass, game_tick;

bool recomp_game_authored_pass(void) { return authored_pass; }
uint32_t recomp_game_pass(void) { return game_pass; }
uint32_t recomp_game_tick(void) { return game_tick; }
unsigned recomp_game_substep(void) { return pass_substep; }

void recomp_game_pass_begin(bool tick, unsigned steps, unsigned substep)
{
    authored_pass = tick;
    pass_steps = tick || steps == 0 ? 1u : steps;
    pass_substep = tick ? 0u : substep;
    ++game_pass;
    if (tick) ++game_tick;
}
static RecompGameFrameReplay frame_owner_draws;

static void record_frame_owner_draw(const struct RecompD3dPresenterCommand *command)
{
    if (!recomp_game_frame_replay_record(&frame_owner_draws, command))
        recomp_stop(1, "game:frame-replay-allocation");
}

static void replay_frame_owner_draws(void)
{
    /* As in the draw adapter, a rejected draw (including after close) is a
       decline, not a runtime failure. */
    for (unsigned i = 0; i < frame_owner_draws.count; ++i) {
        if (recomp_d3d_presenter_submit(recomp_d3d_frame_adapter_presenter(),
                &frame_owner_draws.commands[i]) != RECOMP_D3D_PRESENTER_OK) break;
    }
}

static uint64_t timing_now(void)
{
    return trace_timing ? recomp_xapi_performance_counter() : 0;
}

unsigned recomp_game_requested_hz(void)
{
    /* Manual lookups run on every dispatched call; read the setting once. */
    static int requested = -1;
    if (requested < 0) {
        const char *rate = getenv("RECOMP_GAME_HZ");
        char *end = NULL;
        const unsigned long hz = rate != NULL ? strtoul(rate, &end, 10) : 0;
        requested = end != rate && end != NULL && *end == '\0' && hz >= 60 && hz <= 360 ? (int)hz : 0;
    }
    return (unsigned)requested;
}

bool recomp_game_timing_enabled(void) { return trace_timing; }

unsigned recomp_game_scheduler_hz(void) { return 60u * pass_steps; }

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
    authored_pass = true;
    pass_steps = 1;
    pass_substep = 0;
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
            /* Only drawing fibers run between ticks. UI owner output is
               retained; gameplay, animation and camera controllers never run. */
            const bool replay_owner = callback == 0x0008a970u && game_hz > 60u;
            const bool draw_owner = callback == 0x00079d00u || callback == 0x000b42b0u;
            if (replay_owner && authored_pass) recomp_game_frame_replay_clear(&frame_owner_draws);
            if (replay_owner && !authored_pass) replay_frame_owner_draws();
            unsigned subtick = 0;
            const bool resume = authored_pass ? recomp_game_sleep_step(&ticks, &subtick, 1u) :
                draw_owner && ticks == 0;
            *recomp_memory_u32(slot + 4u) = (uint32_t)ticks;
            if (resume) {
                const uint32_t draws_before = recomp_d3d_draw_adapter_submitted();
                const bool trace = trace_rng && authored_pass;
                const uint32_t rng_before = trace ? recomp_game_debug_rng_state() : 0;
                const uint64_t started = timing_now();
                if (replay_owner) recomp_d3d_draw_adapter_observe(record_frame_owner_draw);
                call_one(0x0018305au, *recomp_memory_u32(slot + 8u));
                recomp_d3d_draw_adapter_observe(NULL);
                if (trace && recomp_game_debug_rng_state() != rng_before) {
                    fprintf(stderr, "recomp rng: tick=%u pass=%u swap=%u slot=%u callback=%08x before=%08x after=%08x\n",
                        game_tick, scheduler_pass, recomp_d3d_frame_adapter_swap_counter(), index, callback,
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
   Those keep their authored duration at the presentation rate. Delivery
   stays in the native D3D adapter. */
static void game_swap(void)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const unsigned presents = main_present_pending ? 1u : (game_hz + 30u) / 60u;
    main_present_pending = false;
    for (unsigned i = 0; i < presents; ++i) {
        recomp_runtime.registers.esp = stack;
        recomp_d3d_swap_adapter();
    }
}

enum { TICK_NS = 1000000000 / 60 };
static bool display_ready;
static int64_t completed_tick_ns;
bool recomp_game_tick_hidden(void)
{
    return display_ready && authored_pass &&
        *recomp_memory_u32(0x005f2b98u) == 0 && !recomp_game_startup_movie_playing();
}
static int64_t next_tick_ns, pass_start_ns, tick_pass_ns, display_pass_ns, render_ns;
static uint64_t pass_start_waited_ns;
/* Vblank waits stretch every pass to a refresh, and presenter-queue waits are
   render cost (render_ns); neither is game work. */
static uint64_t waited_ns(void) { return recomp_d3d_vblank_waited_ns() + recomp_d3d_queue_waited_ns(); }
/* Display passes per tick: all the rate asks for and those run since the
   last tick. pacing.passes is how many currently fit. */
static unsigned display_steps = 1, display_done;
static RecompGamePacing pacing = {1};
static bool timing_tick_pass;

static int64_t now_ns(void) { return (int64_t)recomp_xapi_performance_counter(); }

/* Simulation has its own 60 Hz deadline. Every completed tick gets a
   display; remaining refresh slots use the same two completed snapshots. */
static bool next_pass_is_tick(void)
{
    const int64_t interval = 1000000000 / game_hz;
    int64_t now = now_ns();
    if (pass_start_ns != 0) {
        const int64_t last = now - pass_start_ns - (int64_t)(waited_ns() - pass_start_waited_ns);
        int64_t *average = timing_tick_pass ? &tick_pass_ns : &display_pass_ns;
        *average = *average ? (*average * 7 + last) / 8 : last;
    }
    if (next_tick_ns == 0) next_tick_ns = now;
    const bool display = display_ready && game_hz > 60u &&
        *recomp_memory_u32(0x005f2b98u) == 0 && !recomp_game_startup_movie_playing();
    /* A display pass only fills time before the next tick is due, so a slow
       pass never delays ticks; the passes that miss are dropped. */
    if (display && display_done < pacing.passes && now < next_tick_ns &&
        (timing_tick_pass || now < next_tick_ns - interval / 2)) {
        ++display_done;
        recomp_d3d_count_passes(0, 1, 0);
        pass_start_ns = now;
        pass_start_waited_ns = waited_ns();
        timing_tick_pass = false;
        return false;
    }
    /* Hidden ticks have no vblank wait of their own. */
    while ((now = now_ns()) < next_tick_ns) {
        if (next_tick_ns - now > 2000000) Sleep(1);
        else YieldProcessor();
    }
    pass_start_ns = now;
    pass_start_waited_ns = waited_ns();
    timing_tick_pass = true;
    /* ponytail: a stall longer than four ticks (a load) is not caught up. */
    if (now - next_tick_ns > 4 * (int64_t)TICK_NS) next_tick_ns = now;
    next_tick_ns += TICK_NS;
    if (display) {
        const int64_t frame = (int64_t)recomp_d3d_frame_cost_ns();
        render_ns = render_ns ? (render_ns * 7 + frame) / 8 : frame;
        recomp_game_pacing_display_passes(&pacing, display_steps,
            display_done < pacing.passes ? pacing.passes - display_done : 0,
            tick_pass_ns, display_pass_ns, render_ns);
    }
    recomp_d3d_count_passes(1, 0, display ? display_steps - display_done : 0);
    recomp_d3d_set_display_rate(display && pacing.passes < display_steps ? 60u * pacing.passes : game_hz);
    display_done = 0;
    static int64_t report_ns;
    static uint32_t report_tick, report_pass;
    if (trace_timing && game_tick % 300u == 0) {
        if (report_ns != 0)
            fprintf(stderr, "recomp game ticks: tick=%u ticks_per_s=%.3f passes_per_s=%.1f steps=%u tick_ms=%.2f display_ms=%.2f render_ms=%.2f\n",
                game_tick, (game_tick - report_tick) * 1e9 / (double)(now - report_ns),
                (game_pass - report_pass) * 1e9 / (double)(now - report_ns), pacing.passes,
                tick_pass_ns / 1e6, display_pass_ns / 1e6, render_ns / 1e6);
        report_ns = now; report_tick = game_tick; report_pass = game_pass;
    }
    return true;
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
    bool displayed = false;

    for (;;) {
        uint64_t timed = timing_now();
        const bool tick = next_pass_is_tick();
        phase_time[6] += timing_now() - timed;
        const uint64_t rollback_started = timing_now();
        if (displayed) recomp_game_rollback_restore();
        phase_time[7] += timing_now() - rollback_started;
        displayed = !tick;
        recomp_game_pass_begin(tick, 1, 0);
        if (!tick) {
            const float alpha = (float)((now_ns() - completed_tick_ns) / (double)TICK_NS);
            recomp_game_display_apply(alpha);
            /* Joint deformation reads a second bone bank that 0x000AE3F0 derives
               from the first; rebuild it from the interpolated bones, or joints crack. */
            const uint32_t saved_eax = recomp_runtime.registers.eax;
            for (unsigned actor = 0; actor < 4; ++actor) {
                recomp_runtime.registers.eax = actor;
                call(0x000ae3f0u);
            }
            recomp_runtime.registers.eax = saved_eax;
            *recomp_memory(0x004d56efu, 1) = 1;
            recomp_game_camera_display();
        }
        if (tick) call(0x000b57d0u);
        call(0x000b5870u); /* Derive edges and repeats; none without a poll. */
        if (tick) call(0x000b5aa0u); /* Service feedback. */
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
        /* The game's sound service counts voice ends and delays in calls, so
           it runs once per tick. The match introduction advances shots from
           those counts. */
        if (tick) call(0x000bab70u);
        phase_time[3] += timing_now() - timed;
        timed = timing_now();
        recomp_game_display_trace(tick);
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
        if (tick && display_ready) {
            const uint64_t commit_started = timing_now();
            recomp_game_display_commit();
            recomp_game_rollback_commit();
            completed_tick_ns = now_ns();
            phase_time[7] += timing_now() - commit_started;
        }
        phase_time[5] += timing_now() - timed;
        if (trace_timing && scheduler_pass % 300 == 0) {
            fprintf(stderr, "recomp game timing: pass=%u phase_ms=", scheduler_pass);
            for (unsigned i = 0; i < 8; ++i) fprintf(stderr, "%s%.3f", i ? "," : "", phase_time[i] / 1e6);
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
    display_ready = game_hz > 60u && recomp_game_rollback_begin();
    display_steps = (game_hz + 59u) / 60u;
    recomp_game_pacing_init(&pacing, display_steps);
    recomp_d3d_set_display_rate(game_hz);
    fprintf(stderr, "recomp game scheduler: hand source, hz=%u display_passes=%d\n", game_hz, display_ready);
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
    static int enabled = -1;
    if (enabled < 0) {
        const char *setting = getenv("RECOMP_GAME_SCHEDULER");
        enabled = (setting != NULL && strcmp(setting, "1") == 0) || recomp_game_requested_hz() != 0;
    }
    if (!enabled) {
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
