#include "game_actor.h"
#include "game_actor_transition.h"
#include "game_scheduler.h"

static float pose_tick_sample[8], pose_tick_cursor[8];
static bool pose_cursor_ready[8];

RecompGameClipAdvance recomp_game_clip_advance(float cursor, double increment,
    float epsilon, unsigned steps_per_tick)
{
    const double advanced = increment + (double)epsilon / steps_per_tick + cursor;
    const RecompGameClipAdvance result = {(float)advanced, (int32_t)advanced};
    return result;
}

float recomp_game_pose_display_time(float tick_sample, float tick_cursor,
    unsigned substep, unsigned steps, bool valid, float fallback)
{
    if (!valid) return fallback;
    if (tick_cursor < tick_sample) return tick_sample;
    return (float)(tick_sample + ((double)tick_cursor - tick_sample) * substep / steps);
}

static uint8_t byte(uint32_t address) { return *recomp_memory(address, 1); }
static uint32_t word(uint32_t address) { return *recomp_memory_u32(address); }
static void set_byte(uint32_t address, uint8_t value) { *recomp_memory(address, 1) = value; }
static float scalar(uint32_t address)
{
    float value;
    recomp_guest_load(&value, address, sizeof value);
    return value;
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
static void cdecl_one(uint32_t frame, uint32_t address, uint32_t argument)
{
    push(argument);
    call(frame, address);
    recomp_runtime.registers.esp += 4;
}

static void layer_clock(void)
{
    const uint32_t actor = word(recomp_runtime.registers.esp + 4u);
    const uint32_t layer = 0x005e0800u + (actor * 2u + recomp_runtime.registers.eax) * 0x80u;
    const uint32_t record = 0x004d6ff0u + actor * 0xbcu;
    const uint32_t work = 0x005e0688u + actor * 0x5cu;
    const unsigned steps = recomp_game_scheduler_hz() / 60u;
    *recomp_memory_u32(0x005e4800u) = layer;
    /* The authored tick tests the clip end before advancing a whole tick.
       Testing it on every substep would start the next clip early. */
    const bool tick_start = recomp_game_substep() == 0;
    if (byte(layer + 0x70u) == 0) {
        if (byte(layer + 0x78u) != 0) {
            set_byte(layer + 0x71u, 0);
            set_byte(layer + 0x78u, 0);
            set_byte(layer + 0x73u, 0);
        } else if (tick_start && (int32_t)word(layer + 0x38u) >= *recomp_memory_u16(layer + 0x64u)) {
            set_byte(layer + 0x73u, 0);
        }
        if (byte(layer + 0x73u) == 1 || byte(layer + 0x71u) == 1) {
            const double rate = (double)scalar(layer + 0x3cu) * scalar(record + 0x2cu) *
                                scalar(record + 0x28u) * scalar(0x004d6fc4u);
            const RecompGameClipAdvance next = recomp_game_clip_advance(
                scalar(layer + 0x34u), rate, scalar(0x00281318u), steps);
            recomp_guest_store(layer + 0x34u, &next.cursor, sizeof next.cursor);
            *recomp_memory_u32(layer + 0x38u) = (uint32_t)next.frame;
            *recomp_memory_u32(layer + 0x44u) = word(layer + 0x38u);
            *recomp_memory_u32(layer + 0x48u) = word(layer + 0x34u);
        }
        if (byte(layer + 0x73u) == 0 && byte(layer + 0x71u) == 0) {
            set_byte(record + 0x54u, 1);
            if (byte(layer + 0x76u) != 0) {
                set_byte(layer + 0x76u, 2);
                set_byte(layer + 0x71u, 0);
                set_byte(layer + 0x72u, 1);
                set_byte(layer + 0x73u, 0);
                set_byte(layer + 0x74u, 0);
                recomp_guest_memset(layer, 0, 0x18u);
                set_byte(work + 0x43u, 0);
                set_byte(work + 0x4bu, 1);
            } else if (byte(layer + 0x77u) == 0) {
                set_byte(layer + 0x70u, 1);
                set_byte(work + 0x48u, byte(work + 0x49u));
            }
        }
        if (byte(layer + 0x70u) != 0 && (byte(layer + 0x30u) & 1u) == 0) {
            set_byte(record + 0x4fu, 1);
            const int32_t command = (int16_t)*recomp_memory_u16(layer + 0x66u);
            if (command != 0) {
                *recomp_memory_u32(0x005e6140u + actor * 0x180u) =
                    (uint32_t)(byte(layer + 0x79u) != 0 ? -command : command);
                *recomp_memory_u16(layer + 0x66u) = 0;
            }
        }
    }
    recomp_runtime.registers.esp += 8u;
}

static void actor_animation(void)
{
    const uint32_t actor = recomp_runtime.registers.edi;
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t work = 0x005e0688u + actor * 0x5cu;
    const uint32_t first_layer = 0x005e0800u + actor * 0x100u;
    const unsigned steps = recomp_game_scheduler_hz() / 60u;
    push(recomp_runtime.registers.ecx); push(saved_ebx); push(frame); push(saved_esi);
    recomp_runtime.registers.esi = work;
    recomp_runtime.registers.eax = work;
    *recomp_memory_u32(0x005dfa24u) = work;
    push(actor);
    call(frame, 0x000ac2b0u);
    *recomp_memory_u32(stack - 4u) = first_layer;
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t layer = first_layer + i * 0x80u;
        recomp_runtime.registers.ebx = layer;
        *recomp_memory_u32(0x005e4800u) = layer;
        if (byte(layer + 0x72u) != 0) cdecl_one(actor * 0x100u, 0x000b21d0u, actor);
        if (byte(layer + 0x71u) != 0) {
            recomp_runtime.registers.eax = actor;
            call(actor * 0x100u, 0x000b2230u);
        }
    }
    recomp_runtime.registers.ecx = actor;
    call(actor * 0x100u, 0x000ac550u);
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t layer = first_layer + i * 0x80u;
        *recomp_memory_u32(0x005e4800u) = layer;
        if (byte(layer + 0x74u) != 0) {
            *recomp_memory_u32(layer + 0x34u) = word(layer + 0x48u);
            *recomp_memory_u32(layer + 0x38u) = (uint32_t)(int32_t)scalar(layer + 0x48u);
            set_byte(layer + 0x74u, 0);
            set_byte(layer + 0x73u, 1);
            cdecl_one(layer, 0x000aec80u, actor);
            pose_cursor_ready[actor * 2u + i] = false;
        }
        if (byte(layer + 0x73u) != 0) {
            const uint32_t cursor_word = word(layer + 0x48u);
            const unsigned slot = actor * 2u + i;
            const float cursor = scalar(layer + 0x48u);
            if (steps > 1) {
                if (recomp_game_substep() == 0) pose_tick_cursor[slot] = cursor;
                const float sample = recomp_game_pose_display_time(pose_tick_sample[slot],
                    pose_tick_cursor[slot], recomp_game_substep() + 1u, steps,
                    pose_cursor_ready[slot], cursor);
                recomp_guest_store(layer + 0x48u, &sample, sizeof sample);
            } else {
                pose_tick_sample[slot] = cursor;
                pose_cursor_ready[slot] = true;
            }
            cdecl_one(layer, 0x000aeef0u, actor);
            *recomp_memory_u32(layer + 0x48u) = cursor_word;
        }
    }
    const uint32_t last_layer = first_layer + 0x80u;
    recomp_runtime.registers.ebx = work;
    recomp_runtime.registers.eax = actor;
    call(last_layer, 0x000ac690u);
    recomp_runtime.registers.ecx = work;
    recomp_runtime.registers.eax = actor;
    call(last_layer, 0x000ac750u);
    const uint32_t pose = word(work + 0x18u);
    const uint8_t contact = scalar(pose + 0xa4u) < scalar(pose + 0x84u);
    set_byte(0x005e5fa8u + actor, contact);
    if (byte(work + 0x43u) != 0) set_byte(0x004d7040u + actor * 0xbcu, contact);
    const uint8_t state = byte(work + 0x51u);
    if (state == 1 || state == 2) {
        set_byte(0x004d7043u + actor * 0xbcu, state == 1);
        set_byte(work + 0x51u, state == 1 ? 2 : 0);
    }
    for (unsigned i = 0; i < 2; ++i) {
        recomp_runtime.registers.eax = i;
        push(actor);
        call(last_layer, 0x000adfb0u);
    }
    *recomp_memory_u32(0x005dfa24u) = 0;
    if (recomp_game_substep() + 1u == steps && *recomp_memory_u16(work + 0x56u) > 0)
        --*recomp_memory_u16(work + 0x56u);
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = actor;
    recomp_runtime.registers.ecx = first_layer;
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp = stack + 4u;
}

static void actor_update(void)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    push(saved_ebx); push(saved_esi); push(saved_edi);
    for (unsigned actor = 0; actor < 4; ++actor) {
        if (byte(0x004d6fd0u + actor) == 0) continue;
        recomp_runtime.registers.ebx = actor;
        recomp_runtime.registers.esi = 0x004d6ff1u + actor * 0xbcu;
        const uint8_t type = byte(recomp_runtime.registers.esi);
        if (type == 10) {
            set_byte(0x005e5fb0u + actor, 0);
        } else {
            recomp_runtime.registers.edx = byte(0x004d6fc0u) == 1 ?
                byte(0x002f5f54u + type) : actor;
            call(frame, 0x0009d0c0u);
            set_byte(0x005e5fb0u + actor, (uint8_t)recomp_runtime.registers.eax);
        }
        recomp_runtime.registers.edi = actor;
        call(frame, byte(0x004d56dau) == 5 ? 0x000acf10u : 0x000ac100u);
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp = stack + 4u;
}

RecompFunction recomp_game_actor_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x000ac070u: return actor_update;
    case 0x000ac100u: return actor_animation;
    case 0x000adfb0u: return layer_clock;
    default: return recomp_game_actor_transition_lookup_manual(address);
    }
}
