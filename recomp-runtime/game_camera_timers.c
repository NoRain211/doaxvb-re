#include "game_camera.h"
#include "game_scheduler.h"

static unsigned script_phase[5];
static unsigned queue_phase[5];

static uint8_t byte(uint32_t address) { return *recomp_memory(address, 1); }
static uint32_t word(uint32_t address) { return *recomp_memory_u32(address); }
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

static void camera_script(void)
{
    const uint32_t camera = recomp_runtime.registers.esi;
    const uint32_t entry_stack = recomp_runtime.registers.esp;
    const uint32_t inherited_frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    const unsigned steps = recomp_game_scheduler_hz() / 60u;
    const unsigned index = (camera - 0x0041a800u) / 0xae0u;
    unsigned *phase = &script_phase[index];
    push(inherited_frame);
    const uint32_t frame = recomp_runtime.registers.esp;
    recomp_runtime.registers.esp -= 0x28u;
    push(saved_ebx);
    const uint32_t script = word(camera + 0x2d0u);
    if (script == 0) {
        *phase = 0;
    } else {
        if (*phase == 0 && word(camera + 0x2d4u) == word(script)) {
            const uint32_t scratch = frame - 0x28u;
            recomp_guest_memset(scratch, 0, 0x28u);
            *recomp_memory_u16(scratch + 2u) = 1;
            push(saved_edi);
            const uint32_t mode = word(script + 4u);
            if (mode == 0 || mode == 2 || mode == 3 || mode == 8) {
                recomp_runtime.registers.eax = 0;
                if (mode == 8) {
                    *recomp_memory_u32(scratch + 4u) = 2;
                    *recomp_memory_u32(scratch + 12u) = word(script + 8u);
                    recomp_runtime.registers.eax = scratch;
                }
                recomp_runtime.registers.edx = byte(camera + 0xad9u);
                push(mode == 8 ? 8u : 2u);
                call(frame, 0x0002eb60u);
                recomp_runtime.registers.esp += 4u;
                if (mode == 0) *recomp_memory_u32(camera + 0x2d0u) = 0;
            } else {
                const unsigned target = byte(camera + 0xad9u);
                if (target < 5) {
                    const uint32_t destination = 0x0041a800u + target * 0xae0u;
                    *recomp_memory(destination + 0x421u, 1) = 1;
                    *recomp_memory(destination + 0x420u, 1) = 3;
                    recomp_guest_memset(destination + 0x3f8u, 0, 0x28u);
                }
                *recomp_memory_u32(camera + 0x2d0u) = 0;
            }
            recomp_runtime.registers.esp += 4u;
            if (word(camera + 0x2d0u) != 0) {
                *recomp_memory_u32(camera + 0x2d0u) += 16u;
                ++*recomp_memory_u32(camera + 0x2e0u);
            }
        }
        if (byte(word(camera + 0x5c8u) + 0x47f0u) == 0 && ++*phase == steps) {
            *phase = 0;
            const int32_t elapsed = (int32_t)(word(camera + 0x2d8u) + 1u);
            *recomp_memory_u32(camera + 0x2d8u) = (uint32_t)elapsed;
            if (elapsed >= 1) {
                *recomp_memory_u32(camera + 0x2d8u) = 0;
                ++*recomp_memory_u32(camera + 0x2d4u);
            }
        }
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.edi = saved_edi;
    *recomp_ebp_register() = inherited_frame;
    recomp_runtime.registers.esp = entry_stack + 4u;
}

static void camera_command_queue(void)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t camera = word(stack + 4u);
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    const unsigned steps = recomp_game_scheduler_hz() / 60u;
    unsigned *phase = &queue_phase[(camera - 0x0041a800u) / 0xae0u];
    recomp_runtime.registers.esp -= 0x184u;
    push(frame); push(saved_ebx); push(saved_edi); push(saved_esi);
    const uint32_t scratch = recomp_runtime.registers.esp + 0x14u;
    if ((int8_t)byte(camera + 0xac8u) > 0 &&
        byte(word(camera + 0x5c8u) + 0x47f0u) == 0) {
        if (*phase == 0) {
            unsigned pending = 0;
            for (unsigned i = 0; i < (unsigned)(int8_t)byte(camera + 0xac8u); ++i) {
                const uint32_t command = camera + 0x428u + i * 32u;
                if ((int32_t)word(command + 28u) < 1) {
                    recomp_runtime.registers.edi = command;
                    recomp_runtime.registers.eax = byte(camera + 0xad9u);
                    call(camera, 0x000322f0u);
                } else {
                    recomp_guest_memcpy(scratch + pending++ * 32u, command, 32u);
                }
            }
            *recomp_memory(camera + 0xac8u, 1) = (uint8_t)pending;
            recomp_guest_memcpy(camera + 0x428u, scratch, pending * 32u);
        }
        if (++*phase == steps) {
            *phase = 0;
            for (unsigned i = 0; i < (unsigned)(int8_t)byte(camera + 0xac8u); ++i) {
                --*recomp_memory_u32(camera + 0x444u + i * 32u);
            }
        }
    } else if ((int8_t)byte(camera + 0xac8u) <= 0) {
        *phase = 0;
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp = stack + 4u;
}

RecompFunction recomp_game_camera_timers_lookup_manual(uint32_t address)
{
    if (address == 0x0002f980u) return camera_script;
    if (address == 0x000321a0u) return camera_command_queue;
    return NULL;
}
