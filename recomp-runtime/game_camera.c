#include "game_camera.h"
#include "game_camera_script.h"
#include "game_scheduler.h"

#include <string.h>

static int32_t orbit_remainder[5];
static struct {
    unsigned phase;
    uint32_t previous[14], next[14];
} title_steps[5];

unsigned recomp_game_camera_smoothing_steps(uint32_t camera)
{
    const unsigned steps = recomp_game_scheduler_hz() / 60u;
    const unsigned index = (camera - 0x0041a800u) / 0xae0u;
    if (steps > 1 && index < 5 && *recomp_memory(camera + 0xaccu, 1) == 1) {
        return title_steps[index].phase == 0 ? 1 : 0;
    }
    return steps;
}

int32_t recomp_game_camera_angle_step(int32_t units, unsigned steps, int32_t *remainder)
{
    const int32_t total = units + *remainder;
    const int32_t increment = total / (int32_t)steps;
    *remainder = total % (int32_t)steps;
    return increment;
}

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

static void store_float(uint32_t address, float value)
{
    recomp_guest_store(address, &value, sizeof value);
}

static float load_float(uint32_t address)
{
    float value;
    recomp_guest_load(&value, address, sizeof value);
    return value;
}

static void camera_call(uint32_t frame, uint32_t address)
{
    *recomp_ebp_register() = frame;
    call(address);
}

static void camera_prepare(void)
{
    static uint32_t history[5][4][14];
    static unsigned position[5];
    static uint8_t previous_mode[5];
    static uint32_t previous_age[5];
    static bool ready[5];
    const uint32_t base = recomp_runtime.registers.eax;
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    const unsigned steps = recomp_game_scheduler_hz() / 60u;
    recomp_runtime.registers.esp -= 8u;
    push(saved_ebx); push(frame); push(saved_esi); push(saved_edi);
    recomp_runtime.registers.ebx = base;
    for (unsigned i = 0; i < 5; ++i) *recomp_memory_u16(base + 0x47d8u + i * 2u) = (uint16_t)i;
    if (*recomp_memory(0x005d89b8u, 1) == 1) {
        push(base); camera_call(frame, 0x000300f0u);
        push(base); camera_call(frame, 0x0002f580u);
        recomp_runtime.registers.esp += 8u;
    }
    for (unsigned i = 0; i < 5; ++i) {
        const uint32_t camera = base + i * 0xae0u;
        if (*recomp_memory(camera + 0xad8u, 1) == 0) {
            ready[i] = false;
            continue;
        }
        const uint8_t mode = *recomp_memory(camera + 0xaccu, 1);
        const uint32_t age = *recomp_memory_u32(camera + 0x954u);
        uint32_t staged[14];
        recomp_guest_load(staged, camera + 0x180u, sizeof staged);
        if (steps > 1 && (mode == 8 || mode == 1)) {
            /* Publication consumes the preceding authored tick's camera.
               A smaller tick must not shorten that existing pipeline delay. */
            if (!ready[i] || previous_mode[i] != mode || age < previous_age[i]) {
                for (unsigned j = 0; j < steps - 1u; ++j) memcpy(history[i][j], staged, sizeof staged);
                position[i] = 0;
                ready[i] = true;
            }
            recomp_guest_store(camera, history[i][position[i]], sizeof staged);
            memcpy(history[i][position[i]], staged, sizeof staged);
            position[i] = (position[i] + 1u) % (steps - 1u);
        } else {
            recomp_guest_store(camera, staged, sizeof staged);
            ready[i] = false;
        }
        previous_mode[i] = mode;
        previous_age[i] = age;
        if (mode == 20) recomp_guest_memcpy(camera + 0x38u, camera + 0x28cu, 16u);
    }
    for (unsigned actor = 0; actor < 4; ++actor) {
        const uint32_t state = 0x004d704cu + actor * 0xbcu;
        if (*recomp_memory(base + 0x36c6u + actor * 20u, 1) !=
            *recomp_memory(state - 0x5bu, 1) || *recomp_memory(state, 1) != 0) {
            for (unsigned i = 0; i < 64; ++i) {
                recomp_guest_memcpy(base + 0x3704u + actor * 0x400u + i * 16u,
                                    0x004d6700u + actor * 0x200u, 16u);
            }
        }
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp = stack + 4u;
}

static void camera_tick(void)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ecx = recomp_runtime.registers.ecx;
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    push(saved_ecx);
    recomp_runtime.registers.eax = 0x0041a800u;
    camera_call(frame, 0x00020ff0u);
    const bool paused = *recomp_memory(0x004d56efu, 1) != 0 ||
        *recomp_memory(0x0041eff1u, 1) != 0;
    *recomp_memory(0x0041eff0u, 1) = paused;
    if (paused) *recomp_memory(0x0041eff1u, 1) = 0;
    push(saved_ebx); push(saved_esi); push(saved_edi);
    recomp_runtime.registers.eax = 0x0041a800u;
    camera_call(frame, 0x0002f070u);
    for (uint32_t item = 0x0041efd8u; item < 0x0041efe2u; item += 2) {
        const int16_t index = (int16_t)*recomp_memory_u16(item);
        const uint32_t camera = 0x0041a800u + index * 0xae0u;
        recomp_runtime.registers.ebx = item;
        recomp_runtime.registers.edi = camera;
        *recomp_memory_u32(stack - 4u) = item;
        if (*recomp_memory(camera + 0xad8u, 1) == 0) continue;
        push(camera); camera_call(frame, 0x000321a0u);
        recomp_runtime.registers.esp += 4u;
        recomp_runtime.registers.esi = camera;
        camera_call(frame, 0x0002f980u);
        if (*recomp_memory(camera + 0xad6u, 1) != 0) {
            camera_call(frame, 0x0002fbf0u);
            if (*recomp_memory(camera + 0x5c4u, 1) == 0) {
                push(camera); camera_call(frame, 0x00021440u);
                recomp_runtime.registers.esp += 4u;
            } else camera_call(frame, 0x00022040u);
            recomp_runtime.registers.esi = camera;
            camera_call(frame, 0x00021ec0u);
            camera_call(frame, 0x00030070u);
            if (!*recomp_memory(0x0041eff0u, 1)) {
                recomp_runtime.registers.eax = camera;
                camera_call(frame, 0x00030800u);
            }
        } else {
            camera_call(frame, 0x0002fae0u);
            recomp_runtime.registers.esi = camera;
            camera_call(frame, 0x0002fbf0u);
            const bool advance = !*recomp_memory(0x0041eff0u, 1);
            if (advance) {
                camera_call(frame, 0x00023fe0u);
                push(camera); camera_call(frame, 0x000301c0u);
                recomp_runtime.registers.esp += 4u;
            }
            recomp_runtime.registers.ecx = camera;
            camera_call(frame, 0x0002fe80u);
            camera_call(frame, 0x00030070u);
            if (advance) {
                recomp_runtime.registers.eax = camera;
                camera_call(frame, 0x00030800u);
            }
            push(camera); camera_call(frame, 0x00030ac0u);
            recomp_runtime.registers.esp += 4u;
            recomp_runtime.registers.ebx = camera;
            camera_call(frame, 0x00030d20u);
            camera_call(frame, 0x0002ffb0u);
            recomp_runtime.registers.ebx = item;
        }
        recomp_runtime.registers.eax = camera;
        camera_call(frame, 0x00023b10u);
    }
    recomp_runtime.registers.ebx = 0x0041efe2u;
    *recomp_memory_u32(stack - 4u) = recomp_runtime.registers.ebx;
    recomp_runtime.registers.esi = 0x0041a800u;
    camera_call(frame, 0x0002f2b0u);
    recomp_runtime.registers.edi = saved_edi;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.ecx = 0x0041efe2u;
    recomp_runtime.registers.esp = stack + 4u;
}

static void camera_fiber(void)
{
    const uint32_t frame = *recomp_ebp_register();
    camera_call(frame, 0x0002d640u);
    *recomp_memory_u32(0x0041a7ecu) = *recomp_memory_u32(0x004d6fc4u);
    *recomp_memory_u32(0x0041de60u) = 0x0041a7ecu;
    for (unsigned i = 0; i < 5; ++i) *recomp_memory(0x0041a7f0u + i, 1) = 0;
    for (;;) {
        camera_call(frame, 0x0002ef20u);
        const uint32_t slot = 0x005f2c38u + *recomp_memory_u32(0x005f2e5cu) * 32u;
        *recomp_memory_u32(slot) &= ~0x10u;
        *recomp_memory_u32(slot + 4u) = 0;
        push(*recomp_memory_u32(0x005f2e58u));
        camera_call(frame, 0x0018305au);
    }
}

static void stage_camera(void)
{
    const uint32_t camera = recomp_runtime.registers.edi;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const unsigned steps = recomp_game_scheduler_hz() / 60u;
    const unsigned index = (camera - 0x0041a800u) / 0xae0u;
    const bool title = steps > 1 && index < 5 && *recomp_memory(camera + 0xaccu, 1) == 1;
    if (title && title_steps[index].phase != 0) {
        recomp_guest_store(camera + 0x110u, title_steps[index].next, sizeof title_steps[index].next);
    }
    const float radius = load_float(camera + 0x13cu);
    const uint8_t coordinates = *recomp_memory(camera + 0xad4u, 1);
    push(saved_esi);
    recomp_runtime.registers.eax = coordinates;
    recomp_runtime.registers.esi = camera + 0x110u;
    if (!title || title_steps[index].phase == 0) call(0x0002dfe0u);
    if (!title && steps > 1 && coordinates == 1 &&
        *recomp_memory(camera + 0xad2u, 1) != 0 &&
        *recomp_memory(camera + 0xad5u, 1) == 0) {
        /* Spherical output uses the game's approximate trig. Its round-trip
           radius loss is an authored-tick effect, not a second camera motion. */
        const float converted = load_float(camera + 0x13cu);
        const float advanced = radius + (converted - radius) / steps;
        if (converted > 0) {
            for (unsigned axis = 0; axis < 3; ++axis) {
                const uint32_t eye = camera + 0x110u + axis * 4u;
                const float focus = load_float(camera + 0x120u + axis * 4u);
                store_float(eye, (float)(focus +
                    ((double)load_float(eye) - focus) * advanced / converted));
            }
            store_float(camera + 0x13cu, advanced);
        }
    }
    if (title) {
        if (title_steps[index].phase == 0) {
            recomp_guest_load(title_steps[index].next, camera + 0x110u, sizeof title_steps[index].next);
        }
        const unsigned completed = title_steps[index].phase + 1;
        if (completed != steps) {
            for (unsigned i = 0; i < 14; ++i) {
                uint32_t value;
                const uint32_t before = title_steps[index].previous[i];
                const uint32_t after = title_steps[index].next[i];
                if (i >= 8 && i <= 10) {
                    const int32_t delta = (int16_t)(after - before);
                    value = before + delta * (int32_t)completed / (int32_t)steps;
                } else {
                    float a, b, sample;
                    memcpy(&a, &before, 4); memcpy(&b, &after, 4);
                    sample = (float)(a + ((double)b - a) * completed / steps);
                    memcpy(&value, &sample, 4);
                }
                *recomp_memory_u32(camera + 0x110u + i * 4) = value;
            }
        }
        title_steps[index].phase = completed % steps;
    }
    const unsigned fields[] = {0, 4, 8, 0x10, 0x14, 0x18, 0x20, 0x24,
                               0x28, 0x2c, 0x30, 0x34};
    for (unsigned i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
        *recomp_memory_u32(camera + 0x180u + fields[i]) =
            *recomp_memory_u32(camera + 0x110u + fields[i]);
    }
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.esp += 8u;
}

static void orbit_parameters(uint32_t block)
{
    store_float(block + 0x10u, 0);
    store_float(block + 0x14u, 5);
    store_float(block + 0x18u, 0);
    *recomp_memory_u32(block + 0x20u) = 3640; /* Pitch in 16-bit turn units. */
    store_float(block + 0x2cu, 250);
    store_float(block + 0x30u, 0.6195919f); /* 35.5 degrees, in radians. */
    store_float(block + 0x34u, 0);
}

static void title_orbit(void)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t camera = *recomp_memory_u32(stack + 4u);
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    const unsigned index = (camera - 0x0041a800u) / 0xae0u;
    const unsigned steps = recomp_game_scheduler_hz() / 60u;
    push(saved_ebx); push(saved_esi); push(saved_edi);
    recomp_runtime.registers.ebx = camera;
    recomp_runtime.registers.esi = camera + 0x110u;
    recomp_runtime.registers.edi = camera + 0x148u;
    recomp_runtime.registers.eax = 0;
    if (*recomp_memory(camera + 0xacau, 1) != 0) {
        call(0x0002d700u);
        *recomp_memory(camera + 0xaccu, 1) = 1;
        *recomp_memory(camera + 0xad8u, 1) = 1;
        *recomp_memory(camera + 0xad2u, 1) = 1;
        *recomp_memory(camera + 0xad3u, 1) = 0;
        *recomp_memory(camera + 0xad5u, 1) = 0;
        orbit_parameters(camera + 0x110u);
        *recomp_memory_u32(camera + 0x134u) = 0;
        recomp_runtime.registers.eax = 0;
        call(0x0002dfe0u);
        if (index < 5) {
            orbit_remainder[index] = 0;
            title_steps[index].phase = 0;
        }
    }
    if (steps > 1 && index < 5) {
        if (title_steps[index].phase == 0) {
            recomp_guest_load(title_steps[index].previous, camera + 0x110u, sizeof title_steps[index].previous);
        } else {
            recomp_guest_store(camera + 0x110u, title_steps[index].next, sizeof title_steps[index].next);
        }
    }
    int32_t scratch = 0;
    int32_t *remainder = index < 5 ? &orbit_remainder[index] : &scratch;
    const int32_t increment = steps > 1 && index < 5 ?
        (title_steps[index].phase == 0 ? 18 : 0) :
        recomp_game_camera_angle_step(18, steps, remainder);
    orbit_parameters(camera + 0x148u);
    *recomp_memory_u32(camera + 0x16cu) += (uint32_t)increment;
    recomp_runtime.registers.eax = 0;
    recomp_runtime.registers.esi = camera + 0x148u;
    call(0x0002dfe0u);
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    recomp_runtime.registers.esp = stack + 4u;
}

RecompFunction recomp_game_camera_lookup_manual(uint32_t address)
{
    RecompFunction timers = recomp_game_camera_timers_lookup_manual(address);
    if (timers == NULL) timers = recomp_game_camera_script_lookup_manual(address);
    if (timers != NULL) return timers;
    switch (address) {
    case 0x000249f0u: return title_orbit;
    case 0x0002eec0u: return camera_fiber;
    case 0x0002ef20u: return camera_tick;
    case 0x0002f070u: return camera_prepare;
    case 0x00030070u: return stage_camera;
    default: return recomp_game_camera_smoothing_lookup_manual(address);
    }
}
