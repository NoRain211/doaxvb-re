#include "game_actor_transition.h"
#include "game_scheduler.h"

static unsigned transition_phase[4];

bool recomp_game_blend_step(uint16_t *age, unsigned *phase, uint16_t duration,
    unsigned steps_per_tick, float *blend)
{
    if (++*phase == steps_per_tick) {
        *phase = 0;
        ++*age;
    }
    if (*age >= duration) return true;
    *blend = (float)(((double)*age + (double)*phase / steps_per_tick) / duration);
    return false;
}

static uint8_t byte(uint32_t address)
{
    return *recomp_memory(address, 1);
}

static void set_byte(uint32_t address, uint8_t value)
{
    *recomp_memory(address, 1) = value;
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

/* The transition snapshot is still consumed by generated pose composition.
   Keep that guest layout until the composition owner is replaced. */
static void actor_transition(unsigned steps)
{
    const uint32_t actor = recomp_runtime.registers.ecx;
    const uint32_t workspace = recomp_runtime.registers.esi;
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t entry_stack = recomp_runtime.registers.esp;
    unsigned *phase = &transition_phase[actor % 4u];
    /* A tick pass is one whole step; display passes count on from it. */
    if (steps == 1) *phase = 0;
    push(actor);
    push(saved_ebx);

    const uint8_t requested_duration = byte(0x005e5fc8u + actor);
    if (requested_duration != 0) {
        *recomp_memory_u16(workspace + 0x28u) = requested_duration;
        set_byte(workspace + 0x2du, 1);
        set_byte(0x005e5fc8u + actor, 0);
    }
    if (byte(0x00b3c382u) != 0 ||
        byte(0x004d704cu + actor * 0xbcu) != 0) {
        set_byte(0x00b3c382u, 0);
        set_byte(workspace + 0x2du, 0);
        set_byte(workspace + 0x2cu, 0);
        *recomp_memory_u32(workspace + 0x1cu) = 0;
        *phase = 0;
    }
    if (byte(workspace + 0x2du) != 0) {
        const uint32_t source = *recomp_memory_u32(workspace + 0x18u);
        const uint32_t snapshot = 0x005dfb24u + actor * 0x2f4u;
        *recomp_memory_u32(workspace + 0x1cu) = snapshot;
        recomp_guest_memcpy(snapshot + 0x0cu, source + 0x0cu, 0xf0u);
        recomp_guest_memcpy(snapshot, source, 0x0cu);
        *recomp_memory_u32(entry_stack - 4u) = 0;
        const uint32_t angle = *recomp_memory_u32(0x005e613cu + actor * 0x180u) -
                               *recomp_memory_u32(0x004d7004u + actor * 0xbcu);
        if (angle != 0) {
            recomp_runtime.registers.ebx = snapshot;
            push(angle);
            call(frame, 0x000afd90u);
            recomp_runtime.registers.esp += 4;
        }
        *recomp_memory_u16(workspace + 0x2au) = 0;
        set_byte(workspace + 0x2cu, 2);
        set_byte(workspace + 0x4au, byte(workspace + 0x48u));
        set_byte(workspace + 0x2du, 0);
        *phase = 0;
    }
    if (byte(workspace + 0x2cu) != 0) {
        uint16_t age = *recomp_memory_u16(workspace + 0x2au);
        const uint16_t duration = *recomp_memory_u16(workspace + 0x28u);
        float blend;
        const bool complete = recomp_game_blend_step(&age, phase, duration, steps, &blend);
        *recomp_memory_u16(workspace + 0x2au) = age;
        if (complete) {
            set_byte(workspace + 0x2cu, 0);
            *recomp_memory_u32(workspace + 0x1cu) = 0;
            set_byte(workspace + 0x4au, 0);
            *phase = 0;
        } else {
            recomp_guest_store(workspace + 0x20u, &blend, sizeof blend);
            *recomp_memory_u32(entry_stack - 4u) = duration;
        }
    } else {
        *phase = 0;
    }
    const uint8_t contact = byte(workspace + 0x4au);
    const uint32_t pose = *recomp_memory_u32(workspace + 0x18u);
    if (contact == 1 || contact == 2) {
        *recomp_memory_u32(workspace + 0x34u) =
            *recomp_memory_u32(pose + (contact == 1 ? 0xa4u : 0x84u));
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.ecx = *recomp_memory_u32(entry_stack - 4u);
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp = entry_stack + 4u;
}

static void actor_transition_entry(void)
{
    actor_transition(recomp_game_scheduler_hz() / 60u);
}

RecompFunction recomp_game_actor_transition_lookup_manual(uint32_t address)
{
    return address == 0x000ac550u ? actor_transition_entry : NULL;
}
