#include "game_scene.h"

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
    return passed;
}
