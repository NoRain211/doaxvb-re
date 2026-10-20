#include "game_camera.h"

#include "game_scheduler.h"

#include <math.h>

enum {
    CAMERA_GATE = 0xad2,
    CAMERA_MODE = 0xad3,
    CAMERA_UPDATE_MODE = 0xad4,
    CAMERA_COMMIT_MODE = 0xad5,
    CAMERA_EARLY_PHASE = 0x144,
    CAMERA_LATE_PHASE = 0x17c,
    CAMERA_CURRENT_POSITION = 0x110,
    CAMERA_STAGED_POSITION = 0x148,
    CAMERA_BLEND_POSITION = 0x120,
    CAMERA_TARGET_POSITION = 0x158,
};

static float guest_float(uint32_t address)
{
    float value;
    recomp_guest_load(&value, address, sizeof value);
    return value;
}

static void store_guest_float(uint32_t address, float value)
{
    recomp_guest_store(address, &value, sizeof value);
}

static uint8_t guest_byte(uint32_t address)
{
    return *recomp_memory(address, 1u);
}

static void push_guest(uint32_t value)
{
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = value;
}

static void call_guest(uint32_t address, uint32_t camera)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    push_guest(0u);
    recomp_runtime.registers.ebp = camera;
    recomp_dispatch_indirect_site(address, stack, __FILE__, __LINE__);
    recomp_runtime.registers.ebp = camera;
}

static double call_guest_trig(uint32_t address, uint32_t camera, int32_t angle)
{
    push_guest((uint32_t)angle);
    recomp_runtime.registers.eax = (uint32_t)angle;
    call_guest(address, camera);

    const uint32_t top = recomp_runtime.fpu_top & 7u;
    const double value = recomp_runtime.fpu_stack[top];
    recomp_runtime.fpu_top = (top + 1u) & 7u;
    return value;
}

static float divisor_for_steps(float divisor, unsigned steps)
{
    return divisor * (float)steps;
}

float recomp_game_camera_scalar_model(
    float current,
    float target,
    float deadzone,
    float divisor,
    float max_step,
    float disabled_divisor,
    unsigned steps)
{
    if (steps == 0u) steps = 1u;
    if (divisor == disabled_divisor || current == target) return current;

    const float per_call_limit = max_step / (float)steps;
    const float per_call_divisor = divisor_for_steps(divisor, steps);
    const double difference = (double)target - current;

    if (difference > deadzone) {
        double step = (difference - deadzone) / per_call_divisor;
        if (step > per_call_limit) step = per_call_limit;
        if (step < 0) step = 0;
        return (float)((double)current + step);
    }
    if (difference < -deadzone) {
        /* The negative path stores the delta as float before clamping. */
        float step = (float)((difference + deadzone) / per_call_divisor);
        if (step < -per_call_limit) step = -per_call_limit;
        if (step > 0) step = 0;
        return (float)((double)current + step);
    }

    return current;
}

uint32_t recomp_game_camera_angle_model(
    uint32_t current,
    uint32_t target,
    int32_t deadzone,
    float divisor,
    int32_t max_step,
    float disabled_divisor,
    unsigned steps)
{
    if (steps == 0u) steps = 1u;
    if (divisor == disabled_divisor) return current;

    const uint32_t old_angle = current & 0xffffu;
    const uint32_t delta = ((target & 0xffffu) - old_angle) & 0xffffu;
    if (delta == 0u) return old_angle;
    if ((int32_t)delta <= deadzone ||
        65536 - deadzone <= (int32_t)delta) return old_angle;

    const bool move_negative = delta > 0x8000u;
    const int32_t distance = move_negative
        ? (int32_t)(65536u - delta) - deadzone
        : (int32_t)delta - deadzone;
    const float per_call_divisor = divisor_for_steps(divisor, steps);
    long long step = (long long)((double)distance / (double)per_call_divisor);
    const int32_t per_call_limit = max_step > 0
        ? max_step / (int32_t)steps
        : 0;
    if (step > per_call_limit) step = per_call_limit;
    if (step < 0) step = 0;

    const uint32_t updated = move_negative
        ? old_angle - (uint32_t)step
        : old_angle + (uint32_t)step;
    return updated & 0xffffu;
}

static void normalize_phase(uint32_t camera, uint32_t offset)
{
    const double old_value = guest_float(camera + offset);
    const double period = guest_float(0x00281268u);
    const double value = old_value - (int64_t)(old_value * guest_float(0x0028186cu)) * period;
    float normalized = (float)value;
    if (value <= guest_float(0x00281278u)) normalized = (float)(value + period);
    if (normalized > guest_float(0x00281264u)) normalized = (float)(normalized - period);
    store_guest_float(camera + offset, normalized);
}

static void smooth_scalar(
    uint32_t camera,
    uint32_t output_offset,
    uint32_t target_offset,
    uint32_t deadzone_offset,
    uint32_t divisor_offset,
    float max_step,
    unsigned steps)
{
    const float value = recomp_game_camera_scalar_model(
        guest_float(camera + output_offset),
        guest_float(camera + target_offset),
        guest_float(camera + deadzone_offset),
        guest_float(camera + divisor_offset),
        max_step,
        guest_float(0x0028124cu),
        steps);
    store_guest_float(camera + output_offset, value);
}

static void smooth_angle(
    uint32_t camera,
    uint32_t output_offset,
    uint32_t target_offset,
    uint32_t deadzone_offset,
    uint32_t divisor_offset,
    int32_t max_step,
    unsigned steps)
{
    const int32_t divisor_source = (int32_t)*recomp_memory_u32(
        camera + divisor_offset);
    const uint32_t value = recomp_game_camera_angle_model(
        *recomp_memory_u32(camera + output_offset),
        *recomp_memory_u32(camera + target_offset),
        (int32_t)*recomp_memory_u32(camera + deadzone_offset),
        (float)divisor_source,
        max_step,
        guest_float(0x0028124cu),
        steps);
    *recomp_memory_u32(camera + output_offset) = value;
}

static void copy_guest(uint32_t camera, uint32_t destination, uint32_t source,
                       size_t size)
{
    recomp_guest_memcpy(camera + destination, camera + source, size);
}

static void update_orbit_pair(
    uint32_t camera,
    uint32_t current_offset,
    uint32_t target_offset,
    unsigned steps)
{
    const float current_x = guest_float(camera + current_offset);
    const float current_z = guest_float(camera + current_offset + 8u);
    const float target_x = guest_float(camera + target_offset);
    const float target_z = guest_float(camera + target_offset + 8u);
    const double dx = (double)target_x - current_x;
    const double dz = (double)target_z - current_z;
    float distance = (float)sqrt(dx * dx + dz * dz);

    if (distance <= guest_float(0x0028124cu)) return;

    distance = recomp_game_camera_scalar_model(
        distance,
        0.0f,
        guest_float(camera + (current_offset == 0x110u ? 0x1f0u : 0x200u)),
        guest_float(camera + (current_offset == 0x110u ? 0x1b8u : 0x1c8u)),
        0.4f,
        guest_float(0x0028124cu),
        steps);

    const float x_factor = guest_float(0x0028127cu);
    const int32_t angle = (int32_t)(
        atan2((double)(float)dz, (double)(float)(dx * x_factor)) *
        (double)guest_float(0x00281284u));
    const double sine = call_guest_trig(0x0017aa10u, camera, angle);
    const double cosine = call_guest_trig(0x0017a9f0u, camera, angle);

    store_guest_float(camera + current_offset,
                      target_x + sine * distance);
    store_guest_float(camera + current_offset + 8u,
                      target_z - cosine * distance);
}

static void smooth_camera_angles(uint32_t camera, unsigned steps)
{
    smooth_angle(camera, 0x130u, 0x168u, 0x210u, 0x1d8u, 0xb6, steps);
    smooth_angle(camera, 0x134u, 0x16cu, 0x214u, 0x1dcu, 0x2d8, steps);
    smooth_angle(camera, 0x138u, 0x170u, 0x218u, 0x1e0u, 0, steps);
}

static void smooth_shared_scalars(uint32_t camera, unsigned steps)
{
    smooth_scalar(camera, 0x140u, 0x178u, 0x220u, 0x1e8u,
                  0.2617993950843811f, steps);
    smooth_scalar(camera, 0x144u, 0x17cu, 0x224u, 0x1ecu,
                  0.027925269678235054f, steps);
}

static void camera_smoothing_owner(void)
{
    RecompRegisters *registers = &recomp_runtime.registers;
    const uint32_t entry_esp = registers->esp;
    const uint32_t saved_ebp = registers->ebp;
    const uint32_t saved_ebx = registers->ebx;
    const uint32_t saved_esi = registers->esi;
    const uint32_t saved_edi = registers->edi;
    const uint32_t camera = *recomp_memory_u32(entry_esp + 4u);
    const unsigned steps = recomp_game_camera_smoothing_steps(camera);

    if (steps == 0) {
        registers->esp = entry_esp + 4u;
        return;
    }

    registers->eax = (registers->eax & 0xffffff00u) |
                     guest_byte(camera + CAMERA_GATE);
    if (guest_byte(camera + CAMERA_GATE) == 0u) {
        registers->ebp = saved_ebp;
        registers->esp = entry_esp + 4u;
        return;
    }

    registers->esp -= 12u;
    push_guest(saved_ebp);
    push_guest(saved_ebx); push_guest(saved_esi); push_guest(saved_edi);
    registers->ebx = camera + CAMERA_CURRENT_POSITION;
    registers->edi = camera + CAMERA_STAGED_POSITION;
    normalize_phase(camera, CAMERA_EARLY_PHASE);
    normalize_phase(camera, CAMERA_LATE_PHASE);

    registers->eax = guest_byte(camera + CAMERA_UPDATE_MODE);
    registers->esi = camera + CAMERA_CURRENT_POSITION;
    call_guest(0x0002dfe0u, camera);
    registers->eax = guest_byte(camera + CAMERA_UPDATE_MODE);
    registers->esi = camera + CAMERA_STAGED_POSITION;
    call_guest(0x0002dfe0u, camera);

    const uint8_t mode = guest_byte(camera + CAMERA_MODE);
    const uint8_t update_mode = guest_byte(camera + CAMERA_COMMIT_MODE);

    if (mode == 0u) {
        if (update_mode == 0u) {
            update_orbit_pair(camera, CAMERA_BLEND_POSITION,
                              CAMERA_TARGET_POSITION, steps);
            smooth_camera_angles(camera, steps);
            smooth_scalar(camera, 0x13cu, 0x174u, 0x21cu, 0x1e4u,
                          0.5f, steps);
            smooth_scalar(camera, 0x124u, 0x15cu, 0x204u, 0x1ccu,
                          0.4f, steps);
        } else if (update_mode == 1u) {
            update_orbit_pair(camera, CAMERA_CURRENT_POSITION,
                              CAMERA_STAGED_POSITION, steps);
            update_orbit_pair(camera, CAMERA_BLEND_POSITION,
                              CAMERA_TARGET_POSITION, steps);
            smooth_scalar(camera, 0x114u, 0x14cu, 0x1f4u, 0x1bcu,
                          0.4f, steps);
            smooth_scalar(camera, 0x124u, 0x15cu, 0x204u, 0x1ccu,
                          0.4f, steps);
        } else {
            copy_guest(camera, 0x110u, 0x148u, 12u);
            copy_guest(camera, 0x130u, 0x168u, 24u);
        }
        smooth_shared_scalars(camera, steps);
    } else if (update_mode == 0u) {
        copy_guest(camera, 0x120u, 0x158u, 12u);
        copy_guest(camera, 0x130u, 0x168u, 16u);
        copy_guest(camera, 0x140u, 0x178u, 8u);
    } else if (update_mode == 1u) {
        copy_guest(camera, 0x110u, 0x148u, 12u);
        copy_guest(camera, 0x120u, 0x158u, 12u);
        copy_guest(camera, 0x140u, 0x178u, 8u);
    } else {
        copy_guest(camera, 0x110u, 0x148u, 12u);
        copy_guest(camera, 0x130u, 0x168u, 24u);
    }

    registers->eax = guest_byte(camera + CAMERA_COMMIT_MODE);
    registers->esi = camera + CAMERA_CURRENT_POSITION;
    call_guest(0x0002dfe0u, camera);

    registers->ebx = saved_ebx;
    registers->esi = saved_esi;
    registers->edi = saved_edi;
    registers->ebp = saved_ebp;
    registers->esp = entry_esp + 4u;
}

RecompFunction recomp_game_camera_smoothing_lookup_manual(uint32_t address)
{
    return address == 0x000301c0u ? camera_smoothing_owner : NULL;
}
