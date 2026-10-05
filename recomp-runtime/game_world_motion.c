#include "game_world_motion.h"
#include "game_scheduler.h"
#include <math.h>

static unsigned motion_phase;
static float position_start[3], position_end[3];
static uint32_t angle_start[5], angle_end[5];

static uint8_t byte(uint32_t address) { return *recomp_memory(address, 1); }
static uint32_t word(uint32_t address) { return *recomp_memory_u32(address); }
static float scalar(uint32_t address)
{
    float value;
    recomp_guest_load(&value, address, sizeof value);
    return value;
}
static void store(uint32_t address, float value)
{
    recomp_guest_store(address, &value, sizeof value);
}
static void push(uint32_t value)
{
    recomp_runtime.registers.esp -= 4;
    *recomp_memory_u32(recomp_runtime.registers.esp) = value;
}
static void call(uint32_t frame, uint32_t address)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    *recomp_ebp_register() = frame;
    push(0);
    recomp_dispatch_indirect_site(address, stack, __FILE__, __LINE__);
}

/* Discrete path commands retain their authored cadence. The visible position
   and orientation sample each endpoint interval at the requested tick length. */
static void motion_authored_tick(uint32_t frame)
{
    recomp_runtime.registers.eax = 0x00327cb0u;
    call(frame, 0x00078f80u);
    if (byte(0x00327e52u) == 0) return;
    if (byte(0x00327e4eu) != 0) {
        *recomp_memory_u32(0x00327d1cu) = (word(0x00327d1cu) + 364u) & 0xffffu;
        *recomp_memory_u32(0x00327d20u) = (word(0x00327d20u) + 182u) & 0xffffu;
    }
    const uint32_t saved_matrix = word(0x00a241d4u) - 64u;
    *recomp_memory_u32(0x00a241d4u) = saved_matrix;
    recomp_guest_memcpy(saved_matrix, 0x00a24190u, 64u);
    uint32_t previous[4];
    recomp_guest_load(previous, 0x00327cb0u, sizeof previous);
    recomp_runtime.registers.edi = 0x00327d24u;
    call(frame, 0x00178bb0u);
    const uint8_t result = (uint8_t)recomp_runtime.registers.eax;
    *recomp_memory(0x00327e45u, 1) = word(0x00327d40u) == UINT32_MAX || result != 1;
    *recomp_memory(0x00327e4fu, 1) = result == 0;
    recomp_guest_store(0x00327cc0u, previous, sizeof previous);
    recomp_guest_memcpy(0x00327ce0u, 0x00327cd0u, 16u);
    recomp_runtime.registers.esi = 0x00327cb0u;
    recomp_runtime.registers.eax = (recomp_runtime.registers.eax & ~0xffu) | byte(0x00327e4cu);
    call(frame, 0x00079490u);
    recomp_runtime.registers.ebx = (recomp_runtime.registers.ebx & ~0xffu) | byte(0x00327e4cu);
    call(frame, 0x000795b0u);
    for (unsigned i = 0; i < 4; ++i) {
        store(0x00327d00u + i * 4u,
              scalar(0x00327cd0u + i * 4u) - scalar(0x00327ce0u + i * 4u));
    }
    const int32_t speed = (int8_t)byte(0x00327e44u);
    *recomp_memory_u32(0x00327e40u) += (uint32_t)(speed * 65536 / 360);
    if (byte(0x00327e45u) == 0 && word(0x00327e40u) != 0) {
        const int8_t slower = (int8_t)(speed - 2);
        *recomp_memory(0x00327e44u, 1) = slower > 0 ? (uint8_t)slower : 0;
    }
    const uint32_t restore = word(0x00a241d4u);
    *recomp_memory_u32(0x00a241d4u) = restore + 64u;
    ++*recomp_memory_u32(0x00327e48u);
    *recomp_memory(0x00327e4du, 1) = 0;
    recomp_guest_memcpy(0x00a24190u, restore, 64u);
}

static void world_motion(unsigned steps)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    recomp_runtime.registers.esp -= 16u;
    if (byte(0x004d56efu) == 0) {
        if (motion_phase == 0) {
            recomp_guest_load(position_start, 0x00327cd0u, sizeof position_start);
            recomp_guest_load(angle_start, 0x00327d10u, sizeof angle_start);
            motion_authored_tick(frame);
            recomp_guest_load(position_end, 0x00327cd0u, sizeof position_end);
            recomp_guest_load(angle_end, 0x00327d10u, sizeof angle_end);
        }
        ++motion_phase;
        if (motion_phase == steps) {
            recomp_guest_store(0x00327cd0u, position_end, sizeof position_end);
            recomp_guest_store(0x00327d10u, angle_end, sizeof angle_end);
            motion_phase = 0;
        } else {
            const double fraction = (double)motion_phase / steps;
            for (unsigned i = 0; i < 3; ++i) {
                store(0x00327cd0u + i * 4u, (float)(position_start[i] +
                    ((double)position_end[i] - position_start[i]) * fraction));
            }
            for (unsigned i = 0; i < 5; ++i) {
                const int32_t delta = (int16_t)(angle_end[i] - angle_start[i]);
                *recomp_memory_u32(0x00327d10u + i * 4u) =
                    angle_start[i] + (uint32_t)(int32_t)(delta * fraction);
            }
        }
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp = stack + 4u;
}

static void world_motion_entry(void)
{
    world_motion(recomp_game_scheduler_hz() / 60u);
}

static void boats_authored_tick(uint32_t frame)
{
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t angles = 0x00327540u + i * 0x150;
        *recomp_memory_u32(angles) += 182;
        *recomp_memory_u32(angles + 8) += 136;
        if (byte(angles + 0x120)) {
            uint32_t previous[4];
            recomp_guest_load(previous, angles - 32, sizeof previous);
            recomp_runtime.registers.esi = angles;
            recomp_runtime.registers.ebx = angles - 32;
            recomp_runtime.registers.eax = angles - 32;
            recomp_runtime.registers.edi = angles + 12;
            call(frame, 0x00178bb0u);
            if ((recomp_runtime.registers.eax & 0xff) == 0) *recomp_memory(angles + 0x120, 1) = 0;
            recomp_guest_store(angles - 16, previous, sizeof previous);
            const double dx = (double)scalar(angles - 32) - scalar(angles - 16);
            const double dz = (double)scalar(angles - 24) - scalar(angles - 8);
            if (dz != scalar(0x0028124cu)) {
                recomp_runtime.fpu_top = (recomp_runtime.fpu_top + 7u) & 7u;
                recomp_runtime.fpu_stack[recomp_runtime.fpu_top] = atan2(dx, dz) * scalar(0x0028131cu);
                call(frame, 0x001bb968u);
                const uint32_t desired = 0x8000u - recomp_runtime.registers.eax;
                const uint32_t current = word(angles + 4);
                int32_t delta = (int32_t)((desired - current) & 0xffffu);
                if (delta > 0x8000) delta -= 65536;
                *recomp_memory_u32(angles + 4) = delta > 182 ? current + 182 :
                    delta < -182 ? current - 182 : desired;
            }
        }
    }
}

static void boat_motion(unsigned steps)
{
    static unsigned phase;
    static float start_position[3][4], end_position[3][4];
    static uint32_t start_angle[3][3], end_angle[3][3];
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    recomp_runtime.registers.esp -= 20;
    push(saved_ebx); push(saved_esi); push(saved_edi);
    if (!byte(0x004d56efu)) {
        if (phase == 0) {
            for (unsigned i = 0; i < 3; ++i) {
                recomp_guest_load(start_position[i], 0x00327520u + i * 0x150, sizeof start_position[i]);
                recomp_guest_load(start_angle[i], 0x00327540u + i * 0x150, sizeof start_angle[i]);
            }
            boats_authored_tick(frame);
            for (unsigned i = 0; i < 3; ++i) {
                recomp_guest_load(end_position[i], 0x00327520u + i * 0x150, sizeof end_position[i]);
                recomp_guest_load(end_angle[i], 0x00327540u + i * 0x150, sizeof end_angle[i]);
            }
        }
        const double amount = (double)++phase / steps;
        for (unsigned i = 0; i < 3; ++i) {
            if (phase == steps) {
                recomp_guest_store(0x00327520u + i * 0x150, end_position[i], sizeof end_position[i]);
                recomp_guest_store(0x00327540u + i * 0x150, end_angle[i], sizeof end_angle[i]);
            } else {
                for (unsigned axis = 0; axis < 4; ++axis) {
                    store(0x00327520u + i * 0x150 + axis * 4, (float)(start_position[i][axis] +
                        ((double)end_position[i][axis] - start_position[i][axis]) * amount));
                }
                for (unsigned axis = 0; axis < 3; ++axis) {
                    const int32_t delta = (int16_t)(end_angle[i][axis] - start_angle[i][axis]);
                    *recomp_memory_u32(0x00327540u + i * 0x150 + axis * 4) =
                        start_angle[i][axis] + (int32_t)(delta * amount);
                }
            }
        }
        phase %= steps;
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp = stack + 4;
}

static void boat_motion_entry(void) { boat_motion(recomp_game_scheduler_hz() / 60u); }

RecompFunction recomp_game_world_motion_lookup_manual(uint32_t address)
{
    if (address == 0x0007e280u) return boat_motion_entry;
    return address == 0x00078d80u ? world_motion_entry : NULL;
}
