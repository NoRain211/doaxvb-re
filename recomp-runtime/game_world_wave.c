#include "game_world_wave.h"
#include "game_scheduler.h"

#include <string.h>

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
static uint32_t bits(float value)
{
    uint32_t result;
    memcpy(&result, &value, sizeof result);
    return result;
}
static void push(uint32_t value)
{
    recomp_runtime.registers.esp -= 4;
    *recomp_memory_u32(recomp_runtime.registers.esp) = value;
}
static void call(uint32_t frame, uint32_t address)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    push(0);
    *recomp_ebp_register() = frame;
    recomp_dispatch_indirect_site(address, stack, __FILE__, __LINE__);
}
static double trig(uint32_t frame, uint32_t address, double units)
{
    push(bits((float)(units * scalar(0x00281288u))));
    call(frame, address);
    const double result = recomp_runtime.fpu_stack[recomp_runtime.fpu_top];
    recomp_runtime.fpu_top = (recomp_runtime.fpu_top + 1u) & 7u;
    return result;
}
static int32_t truncate_value(uint32_t frame, double value)
{
    recomp_runtime.fpu_top = (recomp_runtime.fpu_top + 7u) & 7u;
    recomp_runtime.fpu_stack[recomp_runtime.fpu_top] = value;
    call(frame, 0x001bb968u);
    return (int32_t)recomp_runtime.registers.eax;
}

static void surface_scroll_model(unsigned steps)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    recomp_runtime.registers.esp -= 12;
    if (!byte(0x004d56efu)) {
        push(saved_ebx); push(saved_esi); push(saved_edi);
        const uint32_t scene = byte(0x004d566au);
        const uint32_t context = 0x00b2d680u + scene * 0xa00;
        const uint32_t counter = word(0x005bb970u);
        recomp_runtime.registers.ebx = context;
        recomp_runtime.registers.esi = counter;
        *recomp_memory_u32(stack - 4) = counter;
        const unsigned offsets[] = {0x12c, 0x130, 0x134, 0x138, 0x19c, 0x1a0};
        const float increments[] = {-0.0075f, -0.005f, -0.0075f, -0.005f, 0.002f, 0.002f};
        for (unsigned i = 0; i < 6; ++i) {
            const uint32_t delta = bits(increments[i] / steps);
            push(i < 4 ? delta : 0);
            push(i < 4 ? 0 : delta);
            push(word(context + offsets[i]));
            call(frame, 0x0017d0a0u);
        }
        const uint32_t half = (uint32_t)((int32_t)counter / 2);
        for (unsigned layer = 1; layer <= 2; ++layer) {
            const unsigned index = (layer == 1 ? half : counter) % 31;
            const uint32_t table = layer == 1 ? 0x00325cf8u : 0x00325d38u;
            const int32_t offset = (int16_t)*recomp_memory_u16(table + index * 2);
            const uint32_t vertex = word(0x00b299d8u + (scene * 0x190 + (uint32_t)offset) * 4);
            recomp_runtime.registers.esi = layer;
            for (unsigned i = 0; i < 2; ++i) {
                recomp_runtime.registers.edi = vertex;
                recomp_runtime.registers.eax = word(context + 0x19c + i * 4);
                call(frame, 0x0017d130u);
            }
        }
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp = stack + 4;
}

static void wind_strength_model(unsigned steps)
{
    static unsigned phase;
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    recomp_runtime.registers.esp -= 8;
    if (!byte(0x004d56efu)) {
        push(saved_esi);
        if (steps == 1) phase = 0;
        const unsigned completed = phase + 1;
        for (uint32_t record = 0x005c3770u; record < 0x005c37b8u; record += 8) {
            recomp_runtime.registers.esi = record + 4;
            uint16_t age = *recomp_memory_u16(record + 4);
            const double fractional_age = age + (double)completed / steps;
            if (completed == steps) *recomp_memory_u16(record + 4) = ++age;
            const double angle = fractional_age >= 65536 ? fractional_age - 65536 : fractional_age;
            const double cosine = trig(frame, 0x0017aad0u, angle * 2);
            const float envelope = (float)((cosine + scalar(0x00281250u)) * scalar(0x0028128cu));
            const double sine = trig(frame, 0x0017aa30u, angle);
            const double product = (sine + scalar(0x00281250u)) * scalar(0x0028128cu) * envelope;
            double strength = product + product + scalar(0x00281418u);
            if (strength > scalar(0x00281250u)) strength = scalar(0x00281250u);
            if (strength < scalar(0x0028124cu)) strength = scalar(0x0028124cu);
            store(record, (float)strength);
        }
        phase = completed % steps;
    }
    recomp_runtime.registers.esi = saved_esi;
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp = stack + 4;
}

static void foliage_motion_model(unsigned steps)
{
    static unsigned phase;
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t caller_frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    recomp_runtime.registers.esp -= 12;
    if (!byte(0x004d56efu)) {
        call(caller_frame, 0x0007cb60u);
        push(saved_ebx); push(caller_frame); push(saved_esi); push(saved_edi);
        for (unsigned group = 0; group < 2; ++group) {
            const uint32_t descriptor = word(0x00326fa0u + group * 4);
            for (unsigned item = 0; item < *recomp_memory_u16(descriptor + 16); ++item) {
                const uint32_t record = word(descriptor) + item * 64;
                recomp_runtime.registers.ebx = item * 64;
                recomp_runtime.registers.esi = record;
                recomp_runtime.registers.edi = descriptor;
                const unsigned kind = byte(record + 63) & 15;
                if (kind >= 4) continue;
                const float strength = scalar(0x005c3770u + kind * 8);
                if (strength != scalar(0x0028124cu)) {
                    const uint32_t rate = ((uint32_t)byte(record + 62) * 65536u) / 360u;
                    const uint32_t increment = rate * (phase + 1) / steps - rate * phase / steps;
                    const uint16_t angle = (uint16_t)(*recomp_memory_u16(record + 60) + increment);
                    *recomp_memory_u16(record + 60) = angle;
                    const double first = trig(item, 0x0017aa30u, angle);
                    *recomp_memory_u32(record + 44) = (uint32_t)truncate_value(item, first * scalar(0x00281724u));
                    const double second = trig(item, 0x0017aa30u, (angle + 0x2aaa) & 0xffff);
                    *recomp_memory_u32(record + 56) = (uint32_t)truncate_value(item, second * scalar(0x00281720u));
                }
                if (strength != scalar(0x00281250u)) {
                    *recomp_memory_u32(record + 44) = (uint32_t)truncate_value(item, (double)(int32_t)word(record + 44) * strength);
                    *recomp_memory_u32(record + 56) = (uint32_t)truncate_value(item, (double)(int32_t)word(record + 56) * strength);
                }
            }
        }
        phase = (phase + 1) % steps;
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    *recomp_ebp_register() = caller_frame;
    recomp_runtime.registers.esp = stack + 4;
}

static void surface_scroll(void) { surface_scroll_model(recomp_game_scheduler_hz() / 60u); }
static void wind_strength(void) { wind_strength_model(recomp_game_scheduler_hz() / 60u); }
static void foliage_motion(void) { foliage_motion_model(recomp_game_scheduler_hz() / 60u); }

static void effect_texture_clock_model(unsigned steps)
{
    static unsigned phase;
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    recomp_runtime.registers.esp -= 8;
    if (!byte(0x004d56efu)) {
        push(saved_ebx); push(frame); push(saved_esi); push(saved_edi);
        const unsigned scene = byte(0x004d566au);
        const uint32_t context = 0x00b2d680u + scene * 0xa00;
        phase = (phase + 1) % steps;
        for (unsigned effect = 0; effect < 5; ++effect) {
            const uint32_t record = 0x005c47b0u + effect * 4;
            for (unsigned layer = 0; layer < 2; ++layer) {
                if (phase == 0) {
                    const uint8_t age = byte(record + 2 + layer);
                    const unsigned duration = layer ? 11 : 8;
                    if (age + 1 == duration) {
                        *recomp_memory(record + 2 + layer, 1) = 0;
                        *recomp_memory(record + layer, 1) = (byte(record + layer) + 1) % 18;
                    } else *recomp_memory(record + 2 + layer, 1) = age + 1;
                }
                const unsigned texture = *recomp_memory_u16(0x00327910u + byte(record + layer) * 2);
                const unsigned resource = *recomp_memory_u16((layer ? 0x00327940u : 0x00327934u) + effect * 2);
                recomp_runtime.registers.ebx = effect * 2;
                recomp_runtime.registers.eax = word(context + resource * 4);
                recomp_runtime.registers.edi = word(0x00b299d8u + (scene * 0x190 + texture) * 4);
                recomp_runtime.registers.esi = 1;
                call(record + 3, 0x0017d130u);
            }
        }
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp = stack + 4;
}

static void effect_texture_clock(void) { effect_texture_clock_model(recomp_game_scheduler_hz() / 60u); }

/* Game-owned phase and UV updates. Trig, integer conversion, texture selection
   and vertex-offset library helpers remain generated dispatch seams. */
RecompFunction recomp_game_world_wave_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x0007bbc0u: return surface_scroll;
    case 0x0007cb60u: return wind_strength;
    case 0x0007ce00u: return foliage_motion;
    case 0x0007e770u: return effect_texture_clock;
    default: return NULL;
    }
}
