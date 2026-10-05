#include "game_world_timers.h"

#include "game_scheduler.h"

#include <stdbool.h>
#include <math.h>
#include <string.h>

enum {
    WORLD_PAUSE = 0x004d56ef,
    WORLD_INPUT_STATE = 0x004d72e3,
    WORLD_TICKS_A = 0x005bb970,
    WORLD_TICKS_B = 0x005bb974,
    WORLD_TICKS_C = 0x005bb978,
    WORLD_POSITION_X = 0x005bb980,
    WORLD_POSITION_Y = 0x005bb984,
    WORLD_POSITION_Z = 0x005bb988,
    WORLD_POSITION_W = 0x005bb98c,
    WORLD_ANGLE_BASE = 0x005bb9e8,
    WORLD_ANGLE_STEP = 0x005bb9ec,
    WORLD_MODE = 0x005bb9d6,
    WORLD_CYCLE_COUNTER = 0x005bb9d4,
    WORLD_ANGULAR_POSITION = 0x005bb9dc,
    WORLD_ANGULAR_RATE = 0x005bb9e0,
    WORLD_ANGLE_Y = 0x005bb9e4,
    WORLD_PREVIOUS_INPUT = 0x005bb9d0,
    WORLD_LAST_INPUT = 0x005bb9d1,
    WORLD_INPUT_CHANGED = 0x005bb9d2,
    WORLD_ORIGIN_X = 0x00327ce0,
    WORLD_ORIGIN_Y = 0x00327ce4,
    WORLD_ORIGIN_Z = 0x00327ce8,
    DECORATIVE_TABLE = 0x00327408,
};

enum {
    CRT_TRUNCATE_X87 = 0x001bb968,
    WORLD_SINE = 0x0017aa30,
    WORLD_UPDATE_ORIGIN = 0x0017afc0,
};

typedef struct WorldPose {
    float x;
    float y;
    float z;
    float w;
} WorldPose;

static unsigned world_rate_steps;
static uint32_t world_tick_remainder;
static WorldPose world_motion_start;
static WorldPose world_motion_end;
static bool world_motion_valid;

static unsigned decorative_rate_steps;
static uint32_t decorative_remainder_91;
static uint32_t decorative_remainder_182;

static uint8_t guest_u8(uint32_t address)
{
    return *recomp_memory(address, 1u);
}

static int32_t signed_u16(uint16_t bits)
{
    return (bits & 0x8000u) != 0u
        ? (int32_t)bits - 0x10000
        : (int32_t)bits;
}

static uint16_t guest_u16(uint32_t address)
{
    return *recomp_memory_u16(address);
}

static uint32_t guest_u32(uint32_t address)
{
    return *recomp_memory_u32(address);
}

static float guest_float(uint32_t address)
{
    float value;
    recomp_guest_load(&value, address, sizeof value);
    return value;
}

static void store_float(uint32_t address, float value)
{
    recomp_guest_store(address, &value, sizeof value);
}

static void push_guest_u32(uint32_t value)
{
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = value;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void dispatch_child(uint32_t inherited_ebp, uint32_t address)
{
    const uint32_t call_stack = recomp_runtime.registers.esp;
    push_guest_u32(0u);
    *recomp_ebp_register() = inherited_ebp;
    recomp_dispatch_indirect_site(address, call_stack, __FILE__, __LINE__);
    *recomp_ebp_register() = inherited_ebp;
    recomp_runtime.registers.esp = call_stack;
}

static void push_x87(double value)
{
    recomp_runtime.fpu_top = (recomp_runtime.fpu_top + 7u) & 7u;
    recomp_runtime.fpu_stack[recomp_runtime.fpu_top] = value;
}

static int32_t call_crt_truncate(uint32_t inherited_ebp, double value)
{
    const uint32_t saved_top = recomp_runtime.fpu_top;
    push_x87(value);
    dispatch_child(inherited_ebp, CRT_TRUNCATE_X87);
    const int32_t result = (int32_t)recomp_runtime.registers.eax;
    recomp_runtime.fpu_top = saved_top;
    return result;
}

static double call_float_result(
    uint32_t inherited_ebp,
    uint32_t address,
    float argument)
{
    const uint32_t saved_top = recomp_runtime.fpu_top;
    const uint32_t caller_stack = recomp_runtime.registers.esp;
    push_guest_u32(float_bits(argument));
    dispatch_child(inherited_ebp, address);
    const double result = recomp_runtime.fpu_stack[recomp_runtime.fpu_top];
    recomp_runtime.fpu_top = saved_top;
    recomp_runtime.registers.esp = caller_stack;
    return result;
}

static void call_float_argument(
    uint32_t inherited_ebp,
    uint32_t address,
    float argument)
{
    const uint32_t caller_stack = recomp_runtime.registers.esp;
    push_guest_u32(float_bits(argument));
    dispatch_child(inherited_ebp, address);
    recomp_runtime.registers.esp = caller_stack;
}

static WorldPose load_world_pose(void)
{
    const WorldPose pose = {
        guest_float(WORLD_POSITION_X),
        guest_float(WORLD_POSITION_Y),
        guest_float(WORLD_POSITION_Z),
        guest_float(WORLD_POSITION_W),
    };
    return pose;
}

static void store_world_pose(WorldPose pose)
{
    store_float(WORLD_POSITION_X, pose.x);
    store_float(WORLD_POSITION_Y, pose.y);
    store_float(WORLD_POSITION_Z, pose.z);
    store_float(WORLD_POSITION_W, pose.w);
}

static void update_world_mode4(uint32_t inherited_ebp, float caller_return_word)
{
    const int32_t cycle = signed_u16(guest_u16(WORLD_CYCLE_COUNTER));
    const int32_t phase = cycle % 0x1243;
    float rate = 0.0f;
    if (phase < 0x5dc) {
        rate = -0.13333334f;
    } else if (phase >= 0x8ca && phase <= 0x1210) {
        rate = 0.08421053f;
    }
    store_float(WORLD_ANGULAR_RATE, rate);

    const float angular_position = guest_float(WORLD_ANGULAR_POSITION) + rate;
    store_float(WORLD_ANGULAR_POSITION, angular_position);

    const double direction = atan2(
        (double)guest_float(WORLD_ANGLE_Y), (double)angular_position);
    const double scaled_direction = direction *
        (double)guest_float(0x00281284);
    const int32_t angular_step = call_crt_truncate(
        inherited_ebp, scaled_direction);
    const uint32_t accumulated_angle = guest_u32(WORLD_ANGLE_STEP) +
        (uint32_t)angular_step;
    const uint32_t angle_base = guest_u32(WORLD_ANGLE_BASE);
    *recomp_memory_u32(0x005bb97c) =
        (angle_base + accumulated_angle) & 0xffffu;
    *recomp_memory_u32(WORLD_ANGLE_STEP) = accumulated_angle;

    const uint32_t sine_units =
        ((angle_base + accumulated_angle) & 0x1fffu) << 3;
    const float sine_argument = (float)(
        (double)sine_units * (double)guest_float(0x00281288));
    const double sine = call_float_result(
        inherited_ebp, WORLD_SINE, sine_argument);

    const float origin_x = guest_float(WORLD_ORIGIN_X);
    const float origin_y = guest_float(WORLD_ORIGIN_Y);
    const float origin_z = guest_float(WORLD_ORIGIN_Z);
    const float next_y = (float)(sine *
        (double)guest_float(0x002812ac) +
        (double)guest_float(WORLD_POSITION_Y));
    store_float(WORLD_POSITION_Y, next_y);

    const uint32_t delta = recomp_runtime.registers.esp + 4u;
    store_float(delta, guest_float(WORLD_POSITION_X) - origin_x);
    store_float(delta + 4u, guest_float(WORLD_POSITION_Y) - origin_y);
    store_float(delta + 8u, guest_float(WORLD_POSITION_Z) - origin_z);
    store_float(delta + 12u, guest_float(WORLD_POSITION_W) - caller_return_word);
    recomp_runtime.registers.ecx = delta;
    call_float_argument(inherited_ebp, WORLD_UPDATE_ORIGIN, angular_position);
    store_float(WORLD_POSITION_X, guest_float(delta) + origin_x);
    store_float(WORLD_POSITION_Y, guest_float(delta + 4u) + origin_y);

    const int32_t incremented_cycle = signed_u16(
        (uint16_t)((uint16_t)cycle + 1u));
    const int32_t next_cycle = incremented_cycle % 0x1243;
    *recomp_memory_u16(WORLD_CYCLE_COUNTER) = (uint16_t)next_cycle;

    recomp_runtime.registers.eax =
        (uint32_t)(incremented_cycle / 0x1243);
    recomp_runtime.registers.edx =
        (uint32_t)(incremented_cycle % 0x1243);
    recomp_runtime.registers.ecx = 0x1243u;

    store_float(WORLD_POSITION_Z, guest_float(delta + 8u) + origin_z);
    store_float(WORLD_POSITION_W, caller_return_word + guest_float(delta + 12u));
}

static void reset_world_rate(unsigned steps_per_tick)
{
    if (steps_per_tick == 0u) steps_per_tick = 1u;
    if (world_rate_steps == steps_per_tick) return;

    world_rate_steps = steps_per_tick;
    world_tick_remainder = 0;
    world_motion_valid = false;
}

void recomp_game_world_clock_motion_model(unsigned steps_per_tick)
{
    if (steps_per_tick == 0u) steps_per_tick = 1u;

    const uint32_t entry_stack = recomp_runtime.registers.esp;
    const uint32_t inherited_ebp = *recomp_ebp_register();
    const float caller_return_word = guest_float(entry_stack - 4u);
    recomp_runtime.registers.esp -= 0x24u;

    const uint8_t paused = guest_u8(WORLD_PAUSE);
    recomp_runtime.registers.eax =
        (recomp_runtime.registers.eax & 0xffffff00u) | paused;
    if (paused != 0u) {
        recomp_runtime.registers.esp = entry_stack + 4u;
        *recomp_ebp_register() = inherited_ebp;
        return;
    }

    reset_world_rate(steps_per_tick);
    const unsigned completed_substeps = world_tick_remainder + 1u;
    if (completed_substeps == steps_per_tick) {
        *recomp_memory_u32(WORLD_TICKS_A) = guest_u32(WORLD_TICKS_A) + 1u;
        *recomp_memory_u32(WORLD_TICKS_B) = guest_u32(WORLD_TICKS_B) + 1u;
        *recomp_memory_u32(WORLD_TICKS_C) = guest_u32(WORLD_TICKS_C) + 1u;
    }
    recomp_runtime.registers.eax = guest_u32(WORLD_TICKS_A);
    recomp_runtime.registers.edx = guest_u32(WORLD_TICKS_B);
    recomp_runtime.registers.ecx = guest_u32(WORLD_TICKS_C);
    recomp_runtime.registers.eax =
        (recomp_runtime.registers.eax & 0xffffff00u) |
        guest_u8(WORLD_MODE);

    if (guest_u8(WORLD_MODE) == 4u) {
        if (world_tick_remainder == 0) {
            world_motion_start = load_world_pose();
            update_world_mode4(inherited_ebp, caller_return_word);
            world_motion_end = load_world_pose();
            world_motion_valid = true;
        } else if (!world_motion_valid) {
            world_motion_start = load_world_pose();
            world_motion_end = world_motion_start;
            world_motion_valid = true;
        }

        if (steps_per_tick > 1u) {
            if (completed_substeps == steps_per_tick) {
                store_world_pose(world_motion_end);
            } else {
                const float amount = (float)completed_substeps /
                    (float)steps_per_tick;
                const WorldPose intermediate = {
                    recomp_world_timer_lerp(
                        world_motion_start.x, world_motion_end.x, amount),
                    recomp_world_timer_lerp(
                        world_motion_start.y, world_motion_end.y, amount),
                    recomp_world_timer_lerp(
                        world_motion_start.z, world_motion_end.z, amount),
                    recomp_world_timer_lerp(
                        world_motion_start.w, world_motion_end.w, amount),
                };
                store_world_pose(intermediate);
            }
        }
    } else {
        store_float(WORLD_POSITION_Z, 0.0f);
        store_float(WORLD_POSITION_Y, 0.0f);
        store_float(WORLD_POSITION_X, 0.0f);
        world_motion_valid = false;
    }
    world_tick_remainder = completed_substeps % steps_per_tick;

    const uint8_t previous_input = guest_u8(WORLD_PREVIOUS_INPUT);
    const uint8_t current_input = guest_u8(WORLD_INPUT_STATE);
    const uint8_t input_changed = previous_input != current_input;
    *recomp_memory(WORLD_INPUT_CHANGED, 1u) = input_changed;
    *recomp_memory(WORLD_LAST_INPUT, 1u) = previous_input;
    *recomp_memory(WORLD_PREVIOUS_INPUT, 1u) = current_input;

    recomp_runtime.registers.eax =
        (recomp_runtime.registers.eax & 0xffffff00u) | previous_input;
    recomp_runtime.registers.ecx =
        (recomp_runtime.registers.ecx & 0xffffff00u) | current_input;
    recomp_runtime.registers.edx =
        (recomp_runtime.registers.edx & 0xffffff00u) | input_changed;
    recomp_runtime.registers.esp = entry_stack + 4u;
    *recomp_ebp_register() = inherited_ebp;
}

static void reset_decorative_rate(unsigned steps_per_tick)
{
    if (steps_per_tick == 0u) steps_per_tick = 1u;
    if (decorative_rate_steps == steps_per_tick) return;

    decorative_rate_steps = steps_per_tick;
    decorative_remainder_91 = 0u;
    decorative_remainder_182 = 0u;
}

void recomp_game_decorative_rates_model(unsigned steps_per_tick)
{
    if (steps_per_tick == 0u) steps_per_tick = 1u;

    const uint32_t entry_stack = recomp_runtime.registers.esp;
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;

    if (guest_u8(WORLD_PAUSE) == 0u) {
        reset_decorative_rate(steps_per_tick);
        const uint32_t step_91 = recomp_world_timer_split_units(
            91u, steps_per_tick, &decorative_remainder_91);
        const uint32_t step_182 = recomp_world_timer_split_units(
            182u, steps_per_tick, &decorative_remainder_182);

        for (unsigned group = 0; group < 2u; ++group) {
            const uint32_t descriptor =
                guest_u32(DECORATIVE_TABLE + group * 4u);
            const uint32_t records = guest_u32(descriptor);
            const int32_t count = signed_u16(guest_u16(descriptor + 4u));
            for (int32_t item = 0; item < count; ++item) {
                const uint32_t record = records + 0x24u +
                    (uint32_t)item * 0x2cu;
                *recomp_memory_u32(record) += step_91;
                *recomp_memory_u32(record + 4u) += step_182;
            }
        }
    }

    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    recomp_runtime.registers.esp = entry_stack + 4u;
}

static void world_clock_motion_entry(void)
{
    recomp_game_world_clock_motion_model(recomp_game_scheduler_hz() / 60u);
}

static void decorative_rates_entry(void)
{
    recomp_game_decorative_rates_model(recomp_game_scheduler_hz() / 60u);
}

/* CRT conversion and world math/origin helpers remain generated call seams. */
RecompFunction recomp_game_world_timers_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x0007aaf0u: return world_clock_motion_entry;
    case 0x0007d930u: return decorative_rates_entry;
    default: return NULL;
    }
}
