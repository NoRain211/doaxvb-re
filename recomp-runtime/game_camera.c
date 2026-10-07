#include "game_camera.h"
#include "game_camera_script.h"
#include "game_scheduler.h"

#include <string.h>

static int32_t orbit_remainder[5];
static struct {
    unsigned phase;
    uint32_t previous[14], next[14];
} title_steps[5];

/* Camera blocks (eye, focus, angles, distance, field of view): words 8-10
   are 16-bit angles that take the short way; the others are floats. */
static void camera_block_lerp(uint32_t out[14], const uint32_t before[14],
    const uint32_t after[14], unsigned completed, unsigned steps)
{
    for (unsigned i = 0; i < 14; ++i) {
        if (i >= 8 && i <= 10) {
            const int32_t delta = (int16_t)(after[i] - before[i]);
            out[i] = before[i] + delta * (int32_t)completed / (int32_t)steps;
        } else {
            float a, b, sample;
            memcpy(&a, &before[i], 4); memcpy(&b, &after[i], 4);
            sample = (float)(a + ((double)b - a) * completed / steps);
            memcpy(&out[i], &sample, 4);
        }
    }
}

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
    static uint32_t tick_published[5][14], tick_staged[5][14];
    static uint8_t tick_mode[5];
    static uint32_t tick_age[5];
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
        if (steps > 1) {
            /* Publication consumes the preceding tick's camera. A display
               substep publishes between what the tick pass published and the
               camera that tick staged; a new shot publishes the staged one. */
            if (recomp_game_substep() == 0) memcpy(tick_staged[i], staged, sizeof staged);
            if (ready[i] && tick_mode[i] == mode && age >= tick_age[i]) {
                camera_block_lerp(staged, tick_published[i], tick_staged[i],
                    recomp_game_substep() + 1u, steps);
            }
            recomp_guest_store(camera, staged, sizeof staged);
        } else {
            recomp_guest_store(camera, staged, sizeof staged);
            memcpy(tick_published[i], staged, sizeof staged);
            tick_mode[i] = mode;
            tick_age[i] = age;
            ready[i] = true;
        }
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

/* 0x00099500 raises 0x004D5669 on the tick the match state changes; it
   stays raised until that owner runs again a tick later. Display passes see
   it clear: the director 0x0002F580, entering replay, would otherwise save
   the replay mode it has just applied as the mode to return to. */
static bool hide_repeated_state_edge(void)
{
    return !recomp_game_authored_pass() && *recomp_memory(0x004d5669u, 1) != 0;
}

static void camera_tick(void)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ecx = recomp_runtime.registers.ecx;
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    const uint8_t state_edge = *recomp_memory(0x004d5669u, 1);
    const bool hide_edge = hide_repeated_state_edge();
    if (hide_edge) *recomp_memory(0x004d5669u, 1) = 0;
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
    if (hide_edge) *recomp_memory(0x004d5669u, 1) = state_edge;
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
    if (steps == 1 && index < 5) title_steps[index].phase = 0;
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
            uint32_t block[14];
            camera_block_lerp(block, title_steps[index].previous,
                title_steps[index].next, completed, steps);
            recomp_guest_store(camera + 0x110u, block, sizeof block);
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

/* Replay camera, mode 19 (0x00022F40). Each call of its plan step
   0x00023040 advances the sweep (0x0041A7A0 by the speed at 0x0041A7C8,
   which itself accelerates) and ages the plan at 0x0041A798. Above 60 Hz
   the step runs on the authored pass only; the first call after mode entry
   builds the plan at once. Held passes keep the staged target, which the
   per-pass smoothing still follows. */
static void replay_camera(void)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t camera = *recomp_memory_u32(stack + 4u);
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    const uint32_t staged = camera + 0x148u;
    const uint8_t entry = *recomp_memory(camera + 0xacau, 1);
    push(saved_ebx); push(frame); push(saved_esi); push(saved_edi);
    recomp_runtime.registers.edi = camera;
    recomp_runtime.registers.ebx = 1;
    recomp_runtime.registers.eax = (recomp_runtime.registers.eax & ~0xffu) | entry;
    if (entry != 0) {
        recomp_runtime.registers.eax = camera;
        camera_call(staged, 0x0002da70u);
        *recomp_memory(camera + 0xad5u, 1) = 0;
        *recomp_memory(camera + 0xad2u, 1) = 1;
        *recomp_memory(camera + 0xad3u, 1) = 1;
        *recomp_memory(camera + 0x424u, 1) = 0;
        *recomp_memory(camera + 0x425u, 1) = 1;
        store_float(0x0041a7c8u, 1.0f);
        *recomp_memory_u32(0x0041a798u) = 0;
        *recomp_memory_u32(0x0041a79cu) = 0;
        *recomp_memory_u32(0x0041a7ccu) = 0xffffffffu;
        *recomp_memory_u32(0x0041a7d4u) = 0;
        camera_call(staged, 0x00023a10u);
        recomp_runtime.registers.eax = *recomp_memory(camera + 0xad9u, 1);
        camera_call(staged, 0x000204c0u);
    } else {
        *recomp_memory(camera + 0xad3u, 1) = 0;
    }
    recomp_runtime.registers.esi = *recomp_memory(camera + 0xad9u, 1);
    push(1);
    if (*recomp_memory(0x004d9658u, 1) != 1) {
        recomp_runtime.registers.edx = 1;
    } else if (*recomp_memory(0x004d9661u, 1) != 1) {
        recomp_runtime.registers.edx = 0;
    } else {
        const uint8_t option = *recomp_memory(0x004d9518u, 1);
        recomp_runtime.registers.eax = (recomp_runtime.registers.eax & ~0xffu) | option;
        recomp_runtime.registers.edx = option != 0;
    }
    camera_call(staged, 0x00020bd0u);
    recomp_runtime.registers.esp += 4u;
    recomp_runtime.registers.eax = camera;
    camera_call(staged, 0x0002db70u);
    /* The state machine raises 0x004D5669 on the tick it enters replay (5). */
    if (*recomp_memory(0x004d5669u, 1) != 0 && *recomp_memory(0x004d56dau, 1) == 5)
        *recomp_memory(camera + 0xad3u, 1) = 1;
    *recomp_memory(camera + 0xad5u, 1) = 0;
    float age;
    recomp_guest_load(&age, 0x0041a798u, sizeof age);
    if (recomp_game_authored_pass() || age == 0.0f) {
        push(camera);
        camera_call(staged, 0x00023040u);
        recomp_runtime.registers.esp += 4u;
    }
    recomp_runtime.registers.eax = 1;
    recomp_runtime.registers.esi = staged;
    camera_call(staged, 0x0002dfe0u);
    recomp_runtime.registers.edi = saved_edi;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.ebx = saved_ebx;
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp = stack + 4u;
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
    case 0x00022f40u: return replay_camera;
    case 0x0002eec0u: return camera_fiber;
    case 0x0002ef20u: return camera_tick;
    case 0x0002f070u: return camera_prepare;
    case 0x00030070u: return stage_camera;
    default: return recomp_game_camera_smoothing_lookup_manual(address);
    }
}
