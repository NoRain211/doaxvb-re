#include "game_scene.h"

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

static void next_frame(void)
{
    const uint32_t slot = 0x005f2c38u + *recomp_memory_u32(0x005f2e5cu) * 32u;
    *recomp_memory_u32(slot) &= ~0x10u;
    *recomp_memory_u32(slot + 4u) = 1;
    push(*recomp_memory_u32(0x005f2e58u));
    call(0x0018305au);
}

static void scene_callback(uint8_t scene, unsigned event)
{
    call(*recomp_memory_u32(0x002f5ed0u + (uint32_t)scene * 12u + event * 4u));
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
        for (unsigned i = 0; i < 4; ++i) set_byte(0x004d704cu + i * 188u, 0);
        next_frame();
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
        set_byte(0x005ded55u, byte(0x005dece0u));
        ++*recomp_memory_u32(0x004d967cu);
        scene_callback(byte(0x005dece0u), 1);
        if (byte(0x005ded55u) != byte(0x005dece0u) || byte(0x005ded5cu) != 0) {
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
        scenario_tick();
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

static void actor_frame_fiber(void)
{
    call(0x000b42f0u);
    for (;;) {
        call(0x000b4380u);
        call(0x000b4450u);
        next_frame();
    }
}

/* Complete game-owned scene, authored-script and world-frame owners. Their
   generated D3D SetTransform seam remains at 0x001e36d0; wholesale D3D
   replacement remains open. See the whole-game high-frame-rate research page. */
RecompFunction recomp_game_scene_lookup_manual(uint32_t address)
{
    if (address != 0x000a37f0u && address != 0x0006adb0u &&
        address != 0x00079d00u && address != 0x000b42b0u) {
        return NULL;
    }
    const char *enabled = getenv("RECOMP_GAME_SCENE");
    if (enabled == NULL || strcmp(enabled, "1") != 0) return NULL;
    switch (address) {
    case 0x000a37f0u: return scene_fiber;
    case 0x0006adb0u: return scenario_fiber;
    case 0x00079d00u: return world_fiber;
    case 0x000b42b0u: return actor_frame_fiber;
    default: return NULL;
    }
}
