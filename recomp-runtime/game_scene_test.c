#include "game_scene.h"
#include "game_scheduler.h"

#include <stdio.h>
#include <string.h>

static int passed;
static unsigned phase;
static uint32_t camera;

static void expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "game scene: %s\n", message);
        passed = 0;
    }
}

static void return_void(unsigned arguments)
{
    recomp_runtime.registers.esp += 4u + arguments * 4u;
}

static void update_camera(void)
{
    expect(phase++ == 0 && recomp_runtime.registers.eax == camera &&
           recomp_runtime.registers.esi == camera,
           "camera helper did not receive its EAX pointer");
    expect(*recomp_memory(0x0041efe7u, 1) == 2,
           "camera view was not selected before evaluation");
    recomp_runtime.registers.eax = 0xdeadc0deu;
    return_void(0);
}

static void set_projection(void)
{
    expect(phase++ == 1 && recomp_runtime.registers.eax == 7,
           "projection helper did not receive its EAX mode");
    return_void(0);
}

static void set_transform(void)
{
    const uint32_t states[] = {6, 0, 1};
    const uint32_t matrices[] = {0x004d5680u, 0x004d6f20u, 0x004d95f0u};
    const uint32_t stack = recomp_runtime.registers.esp;
    expect(phase >= 2 && phase <= 4, "unexpected transform order");
    if (phase >= 2 && phase <= 4) {
        expect(*recomp_memory_u32(stack + 4u) == states[phase - 2] &&
               *recomp_memory_u32(stack + 8u) == matrices[phase - 2],
               "transform arguments differ");
    }
    ++phase;
    return_void(2);
}

static void environment(void) { expect(phase++ == 5, "environment order"); return_void(0); }
static void begin_lists(void) { expect(phase++ == 6, "list begin order"); return_void(0); }
static void draw_world(void) { expect(phase++ == 7, "world draw order"); return_void(0); }
static void submit_lists(void) { expect(phase++ == 8, "list submit order"); return_void(0); }

static unsigned scenery_draws, wind_ticks;
static uint32_t scenery_clock;
static void advance_wind(void) { ++wind_ticks; return_void(0); }
static void draw_scenery(void)
{
    ++scenery_draws;
    expect(*recomp_memory_u32(0x00b25940u) == 91, "scenery context was not installed");
    expect(*recomp_memory_u32(recomp_runtime.registers.esp + 4) == 0, "scenery index changed");
    if (!*recomp_memory(0x004d56efu, 1)) {
        ++*recomp_memory_u32(scenery_clock);
    }
    ++*recomp_memory_u32(0x03000014u); /* An ungated rand() consumer. */
    return_void(0);
}

static void check_scenery_clock(void)
{
    static uint8_t scene[256], enabled, kinds[4], table[4], context[4];
    static uint8_t current_context[4], descriptor[8], age[8], sunset_age[8], stack[256];
    static uint8_t thread[0x60], rng_pointer[4], rng[0x18];
    static uint8_t body_points[128 * 32];
    const RecompMemoryRegion regions[] = {
        {0x004d5600u, sizeof scene, scene}, {0x004d6fd4u, 1, &enabled},
        {0x004d92f0u, sizeof kinds, kinds}, {0x002961f0u, sizeof table, table},
        {0x002962c4u, sizeof context, context},
        {0x00b25940u, sizeof current_context, current_context},
        {0x00a197d0u, sizeof age, age},
        {0x00a204c0u, sizeof sunset_age, sunset_age}, {0x02000000u, sizeof descriptor, descriptor},
        {0x01000000u, sizeof stack, stack},
        {0x00000000u, sizeof thread, thread}, {0x00eeb28cu, sizeof rng_pointer, rng_pointer},
        {0x03000000u, sizeof rng, rng}, {0x004d5700u, sizeof body_points, body_points},
    };
    const RecompFunctionEntry functions[] = {
        {0x001792e0u, advance_wind}, {0x0014bbb0u, draw_scenery},
        {0x0016a400u, draw_scenery}, {0x00154a90u, draw_scenery},
    };
    for (unsigned scenery = 0; scenery < 3; ++scenery) {
      scenery_clock = scenery ? 0x00a204c0u : 0x00a197d0u;
      for (unsigned steps = 1; steps <= 4; steps *= 2) {
        memset(scene, 0, sizeof scene);
        memset(age, 0, sizeof age);
        memset(sunset_age, 0, sizeof sunset_age);
        memset(rng, 0, sizeof rng);
        enabled = 1;
        scenery_draws = wind_ticks = 0;
        recomp_runtime_init(regions, 14, NULL, 0, functions, 4);
        recomp_game_scheduler_reset(steps * 60);
        *recomp_memory_u32(0x28u) = 0x30u;
        *recomp_memory_u32(0x00eeb28cu) = 0x03000000u;
        *recomp_memory_u32(0x002961f0u) = 0x02000000u;
        const uint32_t callbacks[] = {0x0014bbb0u, 0x0016a400u, 0x00154a90u};
        *recomp_memory_u32(0x02000004u) = callbacks[scenery];
        *recomp_memory_u32(0x002962c4u) = 91;
        *recomp_memory_u32(0x00b25940u) = 37;
        for (unsigned sample = 0; sample < 60 * steps; ++sample) {
            recomp_runtime.registers.esp = 0x010000e0u;
            recomp_game_pass_begin();
            recomp_game_scenery_draw();
            expect(*recomp_memory_u32(scenery_clock) == (sample + 1) / steps,
                   "scenery age depends on presentation rate");
            expect(*recomp_memory_u32(0x03000014u) == (sample + 1) / steps,
                   "held scenery draw consumed game random numbers");
            expect(recomp_runtime.registers.esp == 0x010000e4u &&
                   *recomp_memory_u32(0x00b25940u) == 37 && !scene[0xef],
                   "scenery leaked stack, context, or pause state");
        }
        expect(scenery_draws == 60 * steps && wind_ticks == 60,
               "scenery draw or wind cadence changed");
        scene[0xef] = 1;
        for (unsigned sample = 0; sample < steps; ++sample) {
            recomp_runtime.registers.esp = 0x010000e0u;
            recomp_game_pass_begin();
            recomp_game_scenery_draw();
            expect(scene[0xef] == 1 && *recomp_memory_u32(scenery_clock) == 60,
                   "scenery advanced or unpaused during pause");
        }
        expect(wind_ticks == 61, "wind's original pause-independent clock changed");
        if (steps > 1) {
            recomp_runtime.registers.esp = 0x010000e0u;
            recomp_game_pass_begin();
            recomp_game_scenery_draw();
            scene[0xef] = 0;
            for (unsigned sample = 1; sample < steps; ++sample) {
                recomp_runtime.registers.esp = 0x010000e0u;
                recomp_game_pass_begin();
                recomp_game_scenery_draw();
            }
            expect(wind_ticks == 62 && *recomp_memory_u32(scenery_clock) == 61,
                   "scenery resume separated the wind and effect clocks");
        }
      }
    }
    recomp_runtime_init(NULL, 0, NULL, 0, NULL, 0);
    recomp_game_scheduler_reset(60);
}

int recomp_game_scene_test(void)
{
    static uint8_t cameras[0x5000];
    static uint8_t world[4];
    static uint8_t slot[4];
    static uint8_t stack[256];
    const RecompMemoryRegion regions[] = {
        {0x0041a800u, sizeof cameras, cameras},
        {0x004d56ccu, sizeof world, world},
        {0x005f2e5cu, sizeof slot, slot},
        {0x01000000u, sizeof stack, stack},
    };
    const RecompFunctionEntry functions[] = {
        {0x00023b10u, update_camera}, {0x0002dda0u, set_projection},
        {0x001e36d0u, set_transform}, {0x000c03e0u, environment},
        {0x0017eef0u, begin_lists}, {0x00079f70u, draw_world},
        {0x0017ef70u, submit_lists},
    };
    memset(cameras, 0, sizeof cameras);
    memset(world, 0, sizeof world);
    passed = 1;
    phase = 0;
    camera = 0x0041a800u + 2u * 2784u;
    recomp_runtime_init(regions, 4, NULL, 0, functions, 7);
    recomp_runtime.registers.esp = 0x010000f0u;
    recomp_runtime.registers.eax = 0x005f2c38u;
    *recomp_memory(0x0041efe7u, 1) = 4;
    *recomp_memory_u32(0x005f2e5cu) = 10;
    recomp_game_scene_draw();
    expect(phase == 0, "disabled world submitted work");
    world[0] = 1;
    world[1] = 2;
    *recomp_memory(camera + 2777u, 1) = 7;
    recomp_game_scene_draw();
    expect(phase == 9 && recomp_runtime.registers.esp == 0x010000f0u &&
           *recomp_memory(0x0041efe7u, 1) == 4 &&
           *recomp_memory_u32(0x005f2e5cu) == 10,
           "world pass did not preserve stack, view, or scheduler slot");
    recomp_runtime_init(NULL, 0, NULL, 0, NULL, 0);
    check_scenery_clock();
    return passed;
}
