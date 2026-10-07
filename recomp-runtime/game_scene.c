#include "game_scene.h"
#include "game_camera.h"
#include "game_environment.h"
#include "game_world.h"
#include "game_actor.h"
#include "game_secondary.h"
#include "game_scheduler.h"
#include "xapi_time_adapter.h"
#include <stdio.h>

#include <stdlib.h>
#include <string.h>

static void push(uint32_t value)
{
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = value;
}

static void call(uint32_t address)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    push(0);
    recomp_dispatch_indirect_site(address, stack, __FILE__, __LINE__);
}

static uint8_t byte(uint32_t address)
{
    return *recomp_memory(address, 1);
}

static void set_byte(uint32_t address, uint8_t value)
{
    *recomp_memory(address, 1) = value;
}

static float scalar(uint32_t address)
{
    float value;
    recomp_guest_load(&value, address, sizeof value);
    return value;
}

static void set_scalar(uint32_t address, float value)
{
    recomp_guest_store(address, &value, sizeof value);
}

static bool startup_movie_playing;

bool recomp_game_startup_movie_playing(void) { return startup_movie_playing; }

static void next_frame(void)
{
    const uint32_t slot = 0x005f2c38u + *recomp_memory_u32(0x005f2e5cu) * 32u;
    *recomp_memory_u32(slot) &= ~0x10u;
    /* These owners produce a frame; zero makes their yield independent of
       an authored integer sleep duration in the unconverted fibres. */
    *recomp_memory_u32(slot + 4u) = 0;
    push(*recomp_memory_u32(0x005f2e58u));
    call(0x0018305au);
}

static void scene_callback(uint8_t scene, unsigned event)
{
    call(*recomp_memory_u32(0x002f5ed0u + (uint32_t)scene * 12u + event * 4u));
}

static void startup_movie(void)
{
    const uint32_t frame = *recomp_ebp_register();
    startup_movie_playing = true;
    recomp_runtime.registers.eax = (recomp_runtime.registers.eax & ~0xffu) | 3u;
    call(0x000a3cd0u);
    recomp_runtime.registers.eax = 0;
    *recomp_ebp_register() = frame;
    call(0x000b4ea0u);
    *recomp_ebp_register() = frame;
    call(0x000b52a0u);
    while (recomp_runtime.registers.eax == 0) {
        bool input = (*recomp_memory_u32(0x005f2e78u) & 0xfu) != 0;
        for (unsigned port = 0; port < 4; ++port) {
            input |= (*recomp_memory_u32(0x005f3090u + port * 0x2cu) & 0xfff0u) != 0;
        }
        if (input) break;
        next_frame();
        ++*recomp_memory_u32(0x004d967cu);
        *recomp_ebp_register() = frame;
        call(0x000b52a0u);
    }
    startup_movie_playing = false;
    *recomp_ebp_register() = frame;
    call(0x000b53d0u);
    recomp_runtime.registers.esp += 4u;
}

static void scene_fiber(void)
{
    push(recomp_runtime.registers.ebx);
    call(0x000a3950u);
    scene_callback(byte(0x005dece0u), 0);
    call(0x000a4ea0u);
    call(0x000a4e00u);
    for (;;) {
        set_byte(0x004d6fc0u, byte(0x005dece0u));
        /* Per-actor event bytes last one tick; a display pass leaves them. */
        if (recomp_game_authored_pass())
            for (unsigned i = 0; i < 4; ++i) set_byte(0x004d704cu + i * 188u, 0);
        next_frame();
        const unsigned steps = recomp_game_scheduler_hz() / 60u;
        const bool tick = recomp_game_authored_pass();
        call(0x000a5030u);
        if (scalar(0x005ded50u) != scalar(0x005ded58u)) {
            const float speed = scalar(0x005decf0u) * scalar(0x005ded50u);
            set_scalar(0x005ded58u, scalar(0x005ded50u));
            set_scalar(0x004d6fc4u, speed);
            if (speed == scalar(0x00281250u)) {
                set_byte(0x004d952du, 0);
                set_byte(0x004d9654u, 60);
            } else {
                set_byte(0x004d952du, 1);
                set_byte(0x004d9654u,
                    (uint8_t)(int)(scalar(0x002813e0u) / scalar(0x005decf0u)));
            }
        }
        if (steps > 1) {
            set_scalar(0x004d6fc4u,
                scalar(0x005decf0u) * scalar(0x005ded50u) / steps);
        }
        set_byte(0x005ded55u, byte(0x005dece0u));
        if (tick) ++*recomp_memory_u32(0x004d967cu);
        scene_callback(byte(0x005dece0u), 1);
        /* Scene changes load and create fibres; the tick pass makes them. */
        if (tick && (byte(0x005ded55u) != byte(0x005dece0u) || byte(0x005ded5cu) != 0)) {
            scene_callback(byte(0x005dece0u), 2);
            scene_callback(byte(0x005ded55u), 0);
            call(0x0009c2f0u);
            set_byte(0x005dece0u, byte(0x005ded55u));
            set_byte(0x005ded5cu, 0);
        }
    }
}

static void scenario_callback(uint8_t scenario, unsigned event)
{
    push(0x004d9944u);
    call(*recomp_memory_u32(0x00328c30u + (uint32_t)scenario * 16u + event * 4u));
    recomp_runtime.registers.esp += 4u;
}

static void scenario_tick(void)
{
    if (byte(0x004d9944u) == 0) return;
    if (*recomp_memory_u32(0x004d994cu) == 0) {
        scenario_callback(byte(0x004d9948u), 1);
        set_byte(0x004d72e2u, byte(0x004d9948u));
    }
    scenario_callback(byte(0x004d9948u), 2);
    if ((int32_t)*recomp_memory_u32(0x004d994cu) < INT32_MAX) ++*recomp_memory_u32(0x004d994cu);
    if ((int32_t)*recomp_memory_u32(0x004d9950u) < INT32_MAX) ++*recomp_memory_u32(0x004d9950u);
    if (byte(0x004d994au) != 0) {
        set_byte(0x004d994au, byte(0x004d994au) - 1);
        if ((int8_t)byte(0x004d994au) < 1 || byte(0x004d9945u) != 0) {
            set_byte(0x004d9534u, 2);
            call(0x0006af90u);
        }
    } else if (byte(0x004d9945u) != 0) {
        scenario_callback(byte(0x004d9948u), 3);
        recomp_runtime.registers.edx = 0x004d9944u;
        call(0x0006ae20u);
        set_byte(0x004d9945u, 0);
        *recomp_memory_u32(0x004d994cu) = 0;
        set_byte(0x004d9946u, 0);
        set_byte(0x004d9534u, 1);
    } else if (byte(0x004d9946u) != 0) {
        recomp_runtime.registers.eax = byte(0x004d9949u);
        call(0x0006adf0u);
        scenario_callback((uint8_t)recomp_runtime.registers.eax, 0);
        set_byte(0x004d9946u, 0);
    }
}

static void scenario_fiber(void)
{
    set_byte(0x004d9534u, 0);
    call(0x00097eb0u);
    for (;;) {
        next_frame();
        if (recomp_game_authored_pass()) scenario_tick();
    }
}

void recomp_game_scene_draw(void)
{
    if (byte(0x004d56ccu) != 0) {
        const uint8_t previous_view = byte(0x0041efe7u);
        const uint8_t view = byte(0x004d56cdu);
        if (view < 5) {
            set_byte(0x0041efe7u, view);
            const uint32_t camera = 0x0041a800u + view * 2784u;
            recomp_runtime.registers.esi = camera;
            recomp_runtime.registers.eax = camera;
            call(0x00023b10u);
            recomp_runtime.registers.eax = byte(camera + 2777u);
            call(0x0002dda0u);
            const uint32_t matrices[] = {0x004d5680u, 0x004d6f20u, 0x004d95f0u};
            const uint32_t states[] = {6, 0, 1};
            for (unsigned i = 0; i < 3; ++i) {
                push(matrices[i]); push(states[i]); call(0x001e36d0u);
            }
        }
        call(0x000c03e0u);
        call(0x0017eef0u);
        call(0x00079f70u);
        call(0x0017ef70u);
        if (previous_view < 5) set_byte(0x0041efe7u, previous_view);
    }
}

static void world_fiber(void)
{
    push(recomp_runtime.registers.ebx);
    push(recomp_runtime.registers.esi);
    for (;;) {
        recomp_game_scene_draw();
        next_frame();
    }
}

static uint64_t render_time[12];
static unsigned render_pass;

static void render_call(uint32_t frame, unsigned part, uint32_t address)
{
    const bool timing = recomp_game_timing_enabled();
    const uint64_t start = timing ? recomp_xapi_performance_counter() : 0;
    const uint32_t rng = recomp_game_debug_rng_state();
    *recomp_ebp_register() = frame;
    call(address);
    if (rng != recomp_game_debug_rng_state()) fprintf(stderr, "recomp render rng: part=%u child=%08x before=%08x after=%08x\n", part, address, rng, recomp_game_debug_rng_state());
    if (timing) render_time[part] += recomp_xapi_performance_counter() - start;
}

/* This owner publishes its inherited guest EBP before each generated call. */
static void actor_frame_draw(void)
{
    const uint32_t frame = *recomp_ebp_register();
    render_call(frame, 0, 0x000c03e0u);
    render_call(frame, 1, 0x0017eef0u);
    if (byte(0x00b3c428u) == 0) set_byte(0x00b3c428u, 1);
    *recomp_memory_u32(0x004ca498u) = 0;
    set_byte(0x004ca4dcu, 0);
    set_byte(0x00b3c429u, 1);
    *recomp_memory_u32(0x00a24790u) = 0x000664a0u;
    *recomp_memory_u32(0x004ca494u) = UINT32_MAX;
    render_call(frame, 2, 0x000c0d50u);
    render_call(frame, 3, 0x001435a0u);
    render_call(frame, 4, 0x000c0fa0u);
    if (byte(0x005e69bau) != 0) {
        push(*recomp_memory_u32(0x005e69b0u));
        render_call(frame, 5, 0x001e3870u);
        set_byte(0x005e69bau, 0);
    }
    render_call(frame, 6, 0x000c11b0u);
    for (unsigned actor = 0; actor < 4; ++actor) {
        push(0); push(actor);
        render_call(frame, 7 + (actor != 0), 0x00063000u);
        if (actor == 0 && byte(0x004d6ff1u) == 10) {
            render_call(frame, 9, 0x001e39e0u);
            *recomp_memory_u32(0x005e69b0u) = recomp_runtime.registers.eax;
            set_byte(0x005e69bau, 1);
        }
    }
    render_call(frame, 10, 0x000c12b0u);
    /* Its layer callbacks advance pause-aware effects once per tick. */
    const bool hold = !recomp_game_authored_pass() && !byte(0x004d56efu);
    if (hold) set_byte(0x004d56efu, 1);
    render_call(frame, 11, 0x0017ef70u);
    if (hold) set_byte(0x004d56efu, 0);
    recomp_runtime.registers.esp += 4u;
    if (recomp_game_timing_enabled() && ++render_pass % 300 == 0) {
        fprintf(stderr, "recomp render timing: pass=%u part_ms=", render_pass);
        for (unsigned i = 0; i < 12; ++i) fprintf(stderr, "%s%.3f", i ? "," : "", render_time[i] / 1e6);
        fprintf(stderr, "\n");
        memset(render_time, 0, sizeof render_time);
    }
}

static void actor_frame_fiber(void)
{
    call(0x000b42f0u);
    uint64_t time[2] = {0};
    for (unsigned pass = 1;; ++pass) {
        const bool timing = recomp_game_timing_enabled();
        uint64_t start = timing ? recomp_xapi_performance_counter() : 0;
        call(0x000b4380u);
        if (timing) {
            const uint64_t now = recomp_xapi_performance_counter();
            time[0] += now - start;
            start = now;
        }
        call(0x000b4450u);
        if (timing) {
            time[1] += recomp_xapi_performance_counter() - start;
            if (pass % 300 == 0) {
                fprintf(stderr, "recomp actor frame timing: pass=%u update_ms=%.3f draw_ms=%.3f\n",
                    pass, time[0] / 1e6, time[1] / 1e6);
                time[0] = time[1] = 0;
            }
        }
        next_frame();
    }
}

void recomp_game_scenery_draw(void)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    push(saved_esi); push(saved_edi);
    const unsigned scene = byte(0x004d56e4u);
    const unsigned kind = byte(0x004d92f0u + scene);
    const uint32_t descriptor = *recomp_memory_u32(0x002961f0u + kind * 4);
    recomp_runtime.registers.edi = scene;
    recomp_runtime.registers.esi = descriptor;
    if (descriptor && *recomp_memory_u32(descriptor + 4)) {
        const uint32_t draw = *recomp_memory_u32(descriptor + 4);
        const uint32_t previous_context = *recomp_memory_u32(0x00b25940u);
        *recomp_memory_u32(0x00b25940u) = *recomp_memory_u32(0x002962c4u);
        push(saved_ebx);
        recomp_runtime.registers.ebx = previous_context;
        if (byte(0x004d6fd4u)) {
            const bool authored_tick = recomp_game_authored_pass();
            const bool effects = draw == 0x0014bbb0u || draw == 0x0016a400u ||
                draw == 0x00154a90u || draw == 0x00163710u;
            /* Effects advance once per tick; a display pass draws them held. */
            const bool hold_effects = !authored_tick && !byte(0x004d56efu) && effects;
            if (hold_effects) set_byte(0x004d56efu, 1);
            *recomp_ebp_register() = frame;
            const uint32_t wind_rng = recomp_game_debug_rng_state();
            if (authored_tick) call(0x001792e0u);
            if (wind_rng != recomp_game_debug_rng_state()) fprintf(stderr, "recomp wind rng: before=%08x after=%08x\n", wind_rng, recomp_game_debug_rng_state());
            push(scene);
            *recomp_ebp_register() = frame;
            const uint32_t scenery_rng = recomp_game_debug_rng_state();
            call(draw);
            if (scenery_rng != recomp_game_debug_rng_state()) fprintf(stderr, "recomp scenery rng: draw=%08x kind=%u before=%08x after=%08x\n", draw, kind, scenery_rng, recomp_game_debug_rng_state());
            recomp_runtime.registers.esp += 4;
            if (hold_effects) set_byte(0x004d56efu, 0);
        }
        *recomp_memory_u32(0x00b25940u) = previous_context;
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp = stack + 4;
}

static void actor_update_fiber(void)
{
    call(0x000af1b0u);
    for (;;) {
        call(0x000af220u);
        if (byte(0x004d56efu) == 0) {
            call(0x000ac070u);
            call(0x000ae1a0u);
            call(0x000adc30u);
            call(0x000af2c0u);
        }
        call(0x0017fd20u);
        next_frame();
    }
}

/* Complete game-owned scene, authored-script and world-frame owners. Their
   generated D3D SetTransform and fence seams remain at 0x001e36d0,
   0x001e3870 and 0x001e39e0; wholesale D3D
   replacement remains open. See the whole-game high-frame-rate research page. */
RecompFunction recomp_game_scene_lookup_manual(uint32_t address)
{
    RecompFunction camera = recomp_game_camera_lookup_manual(address);
    if (camera == NULL) camera = recomp_game_environment_lookup_manual(address);
    if (camera == NULL) camera = recomp_game_world_lookup_manual(address);
    if (camera == NULL) camera = recomp_game_actor_lookup_manual(address);
    if (camera == NULL) camera = recomp_game_secondary_lookup_manual(address);
    if (camera == NULL && address != 0x000a37f0u && address != 0x0006adb0u &&
        address != 0x00079d00u && address != 0x000b42b0u &&
        address != 0x000b4450u && address != 0x000a4e00u &&
        address != 0x000af160u && address != 0x001435a0u) {
        return NULL;
    }
    /* This lookup runs on every dispatched call to a hand owner. */
    static int enabled = -1;
    if (enabled < 0) {
        const char *setting = getenv("RECOMP_GAME_SCENE");
        enabled = (setting != NULL && strcmp(setting, "1") == 0) || recomp_game_requested_hz() != 0;
    }
    if (!enabled) return NULL;
    if (camera != NULL) return camera;
    switch (address) {
    case 0x000a37f0u: return scene_fiber;
    case 0x0006adb0u: return scenario_fiber;
    case 0x00079d00u: return world_fiber;
    case 0x000b42b0u: return actor_frame_fiber;
    case 0x000b4450u: return actor_frame_draw;
    case 0x000a4e00u: return startup_movie;
    case 0x000af160u: return actor_update_fiber;
    case 0x001435a0u: return recomp_game_scenery_draw;
    default: return NULL;
    }
}
