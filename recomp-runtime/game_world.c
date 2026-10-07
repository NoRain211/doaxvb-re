#include "game_world.h"
#include "game_world_timers.h"
#include "game_world_motion.h"
#include "game_world_wave.h"
#include "game_scheduler.h"
#include "game_display.h"
#include "xapi_time_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef RECOMP_D3D_FRAME_ENABLED
#include "d3d_frame_adapter.h"
#include "d3d_draw_adapter.h"
#include "d3d_texture_adapter.h"

static unsigned draw_trace_at(void)
{
    static bool read;
    static unsigned at;
    if (!read) {
        const char *text = getenv("RECOMP_GAME_DRAW_TRACE");
        at = text ? (unsigned)strtoul(text, NULL, 10) : 0;
        read = true;
    }
    return at;
}

static void trace_draw(void)
{
    const unsigned at = draw_trace_at();
    static unsigned ordinal;
    if (recomp_d3d_frame_adapter_swap_counter() + 1 == at && ++ordinal <= 85) {
        fprintf(stderr, "recomp game draw: ordinal=%u", ordinal);
        const uint32_t device = *recomp_memory_u32(0x001f2978u);
        for (unsigned stage = 0; stage < 4; ++stage) {
            const uint32_t texture = *recomp_memory_u32(device + 0xb38 + stage * 4);
            fprintf(stderr, " texture%u=%08x", stage, texture);
        }
        const uint32_t count = *recomp_memory_u32(recomp_runtime.registers.esp + 8);
        const uint32_t indices = *recomp_memory_u32(recomp_runtime.registers.esp + 12);
        const uint32_t vb = *recomp_memory_u32(0x001f2e28u);
        const uint32_t stride = *recomp_memory_u32(0x001f2e20u);
        unsigned max_index = 0;
        for (unsigned i = 0; i < count && i < 262144; ++i) {
            unsigned index = *recomp_memory_u16(indices + i * 2);
            if (index > max_index) max_index = index;
        }
        if (vb && max_index < 32768 && stride <= 128) {
            const uint32_t data = *recomp_memory_u32(vb + 4);
            uint32_t hash = 2166136261u;
            const unsigned size = (max_index + 1) * stride;
            const uint8_t *bytes = recomp_memory(data, size);
            for (unsigned i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
            fprintf(stderr, " vertex_hash=%08x", hash);
        }
        const RecompD3dTextureDesc *desc = recomp_d3d_texture_adapter_stage(0);
        if (desc) {
            const unsigned size = desc->format_byte == 12 || desc->format_byte == 14 || desc->format_byte == 15
                ? recomp_d3d_texture_compressed_mip_span(desc) : desc->width * desc->height * desc->bits_per_pixel / 8;
            if (size && size < 16777216) {
                uint32_t hash = 2166136261u;
                const uint8_t *bytes = recomp_memory(desc->data, size);
                for (unsigned i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
                fprintf(stderr, " texel_hash=%08x", hash);
            }
        }
        for (unsigned depth = 0; ordinal <= 3 && depth < 8; ++depth) {
            uint32_t address;
            const char *source;
            int line;
            if (!recomp_dispatch_frame_at(depth, &address, &source, &line)) break;
            fprintf(stderr, " %08x@%s:%d", address, source ? source : "-", line);
        }
        fprintf(stderr, "\n");
    }
    recomp_d3d_draw_lookup_manual(0x001e78b0u)();
}
#endif

enum {
    WORLD_SCENE = 0x004d566a,
    WORLD_PAUSE = 0x004d56ef,
    WORLD_SCRIPT = 0x00327e4d,
    WORLD_SCROLL_COUNTER = 0x005bb970,
    WORLD_MATRIX_STACK = 0x00a241d4,
    WORLD_MATRIX = 0x00a24190,
    WORLD_RENDER_MATRIX = 0x004d5680,
    WORLD_SAVED_MATRIX = 0x005bb990,
    WORLD_COUNTER = 0x0032745c,
    WORLD_VERTEX_TABLE = 0x00b299d8,
    WORLD_CONTEXTS = 0x00b2d680,
    WORLD_FUNCTION_A = 0x00a2478c,
    WORLD_FUNCTION_B = 0x00a24798,
};

static unsigned counter_remainder;
static struct { uint32_t address, calls; uint64_t elapsed; } call_times[64];
static unsigned call_time_count, timed_world_frames;

static uint8_t guest_u8(uint32_t address)
{
    return *recomp_memory(address, 1u);
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

static void push(uint32_t value)
{
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = value;
}

static uint32_t pop(void)
{
    const uint32_t value = guest_u32(recomp_runtime.registers.esp);
    recomp_runtime.registers.esp += 4u;
    return value;
}

static void call_at_frame(uint32_t frame, uint32_t address)
{
    const bool timed = recomp_game_timing_enabled();
    const uint64_t started = timed ? recomp_xapi_performance_counter() : 0;
    const uint32_t stack = recomp_runtime.registers.esp;
    *recomp_ebp_register() = frame;
    push(0);
    recomp_dispatch_indirect_site(address, stack, __FILE__, __LINE__);
    if (timed) {
        unsigned i = 0;
        while (i < call_time_count && call_times[i].address != address) ++i;
        if (i == call_time_count && i < 64) call_times[call_time_count++].address = address;
        if (i < 64) {
            ++call_times[i].calls;
            call_times[i].elapsed += recomp_xapi_performance_counter() - started;
        }
    }
}

static void copy_matrix(uint32_t destination, uint32_t source)
{
    /* The original MOVAPS/MOVUPS paths differ only in alignment. Both copy
       the same 64 bytes and leave XMM0-XMM3 holding the source matrix. */
    recomp_guest_load(recomp_runtime.xmm, source, 64u);
    recomp_guest_memcpy(destination, source, 64u);
}

static unsigned normalize_steps(unsigned steps_per_tick)
{
    return steps_per_tick != 0u ? steps_per_tick : 1u;
}

static unsigned scheduler_steps_per_tick(void)
{
    return normalize_steps(recomp_game_scheduler_hz() / 60u);
}

static uint16_t world_counter_increment(unsigned steps_per_tick)
{
    const unsigned steps = normalize_steps(steps_per_tick);
    if (steps == 1) counter_remainder = 0;
    const unsigned accumulated = counter_remainder + 22u;
    counter_remainder = accumulated % steps;
    return (uint16_t)(accumulated / steps);
}

static uint32_t scaled_float_argument(uint32_t bits, unsigned steps_per_tick)
{
    steps_per_tick = normalize_steps(steps_per_tick);
    if (steps_per_tick == 1u) return bits;

    float value;
    memcpy(&value, &bits, sizeof value);
    value /= (float)steps_per_tick;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static uint32_t scene_context_base(uint8_t scene)
{
    return WORLD_CONTEXTS + (uint32_t)scene * 0xa00u;
}

static uint32_t signed_half_asr1(uint32_t value)
{
    if ((value & 0x80000000u) != 0u) ++value;
    return (value >> 1) | (value & 0x80000000u);
}

static void world_uv_update_model(unsigned steps_per_tick)
{
    const uint32_t caller_frame = *recomp_ebp_register();
    push(recomp_runtime.registers.ecx);
    const uint8_t scene = guest_u8(WORLD_SCENE);
    recomp_runtime.registers.ecx = scene;
    recomp_runtime.registers.edx =
        (recomp_runtime.registers.edx & 0xffffff00u) | guest_u8(WORLD_PAUSE);
    const uint32_t frame_counter = guest_u32(WORLD_SCROLL_COUNTER);
    recomp_runtime.registers.eax = frame_counter;
    push(recomp_runtime.registers.ebx);
    const uint32_t base = scene_context_base(scene);
    recomp_runtime.registers.ebx = base;
    *recomp_memory_u32(recomp_runtime.registers.esp + 4u) = frame_counter;

    if ((recomp_runtime.registers.edx & 0xffu) == 0u) {
        push(recomp_runtime.registers.esi);
        push(recomp_runtime.registers.edi);

        const uint32_t half_counter = signed_half_asr1(frame_counter);
        const uint32_t first_remainder = half_counter % 31u;
        const int16_t first_offset = (int16_t)guest_u16(
            0x00325cf8u + first_remainder * 2u);
        const uint32_t first_slot = (uint32_t)(int32_t)first_offset +
            (uint32_t)scene * 0x190u;

        recomp_runtime.registers.ecx = (uint32_t)scene * 0x190u;
        recomp_runtime.registers.edx = first_remainder;
        recomp_runtime.registers.esi = 1u;
        recomp_runtime.registers.edi = guest_u32(
            WORLD_VERTEX_TABLE + first_slot * 4u);
        recomp_runtime.registers.eax = guest_u32(base + 0x1e4u);
        call_at_frame(caller_frame, 0x0017d130u);

        recomp_runtime.registers.ecx = guest_u32(base + 0x1e4u);
        push(0u);
        push(scaled_float_argument(0xba03126fu, steps_per_tick));
        push(recomp_runtime.registers.ecx);
        recomp_game_display_uv(*recomp_memory_u32(recomp_runtime.registers.esp));
        call_at_frame(caller_frame, 0x0017d0a0u);

        /* This stack slot is the frame counter saved above at entry ESP-4. */
        recomp_runtime.registers.eax = guest_u32(
            recomp_runtime.registers.esp + 0x0cu);
        const uint32_t second_remainder = recomp_runtime.registers.eax % 31u;
        const int16_t second_offset = (int16_t)guest_u16(
            0x00325d38u + second_remainder * 2u);
        const uint32_t second_slot = (uint32_t)(int32_t)second_offset +
            (uint32_t)scene * 0x190u;

        recomp_runtime.registers.ecx = 31u;
        recomp_runtime.registers.edx = second_remainder;
        recomp_runtime.registers.eax = (uint32_t)scene * 0x190u;
        recomp_runtime.registers.edx = (uint32_t)(int32_t)second_offset +
            recomp_runtime.registers.eax;
        recomp_runtime.registers.esi = 2u;
        recomp_runtime.registers.edi = guest_u32(
            WORLD_VERTEX_TABLE + second_slot * 4u);
        recomp_runtime.registers.eax = guest_u32(base + 0x1ecu);
        call_at_frame(caller_frame, 0x0017d130u);

        recomp_runtime.registers.ecx = guest_u32(base + 0x1ccu);
        push(0u);
        push(scaled_float_argument(0xbba3d70au, steps_per_tick));
        push(recomp_runtime.registers.ecx);
        recomp_game_display_uv(*recomp_memory_u32(recomp_runtime.registers.esp));
        call_at_frame(caller_frame, 0x0017d0a0u);

        recomp_runtime.registers.edi = pop();
        recomp_runtime.registers.esi = pop();
    }

    recomp_runtime.registers.ebx = pop();
    recomp_runtime.registers.ecx = pop();
    recomp_runtime.registers.esp += 4u;
}

static void world_uv_update_entry(void)
{
    world_uv_update_model(scheduler_steps_per_tick());
}

static void draw_indexed_world_items(
    uint32_t frame,
    uint32_t index_table,
    unsigned item_count,
    uint32_t child)
{
    recomp_runtime.registers.esi = 0u;
    for (unsigned i = 0; i < item_count; ++i) {
        recomp_runtime.registers.esi = i * 4u;
        recomp_runtime.registers.edi = guest_u32(index_table + i * 4u);
        call_at_frame(frame, child);
    }
    recomp_runtime.registers.esi = item_count * 4u;
}

static void draw_indexed_vertex_calls(
    uint32_t frame,
    uint32_t index_table,
    unsigned item_count)
{
    recomp_runtime.registers.esi = index_table;
    recomp_runtime.registers.edi = item_count;
    for (unsigned i = 0; i < item_count; ++i) {
        const uint16_t index = guest_u16(index_table + i * 2u);
        recomp_runtime.registers.edx = index;
        recomp_runtime.registers.ecx = guest_u32(
            recomp_runtime.registers.ebx + (uint32_t)index * 4u);
        recomp_runtime.registers.eax = index;
        call_at_frame(frame, 0x0017d520u);
        recomp_runtime.registers.esi += 2u;
        --recomp_runtime.registers.edi;
    }
}

static void world_draw_model(unsigned steps_per_tick)
{
    steps_per_tick = normalize_steps(steps_per_tick);

    const uint32_t caller_ebp = *recomp_ebp_register();
    push(caller_ebp);
    const uint32_t frame = recomp_runtime.registers.esp;
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp -= 0x0cu;
    const uint8_t scene = guest_u8(WORLD_SCENE);
    recomp_runtime.registers.eax = scene;
    push(recomp_runtime.registers.ebx);
    push(recomp_runtime.registers.esi);
    push(recomp_runtime.registers.edi);

    recomp_guest_memcpy(0x005bb9f0u, 0x009d5240u, 0x58u);
    recomp_guest_memcpy(0x005bba48u, 0x009d5298u, 0x58u);
    recomp_runtime.registers.ebx = (uint32_t)scene * 5u;
    recomp_runtime.registers.eax = (recomp_runtime.registers.eax & 0xffffff00u) |
        guest_u8(WORLD_PAUSE);
    recomp_runtime.registers.ebx <<= 9;
    recomp_guest_memcpy(0x005bbaa0u, 0x009d52f0u, 0x58u);
    recomp_runtime.registers.ebx += WORLD_CONTEXTS;
    recomp_guest_memcpy(0x005bbaf8u, 0x009d5348u, 0x58u);
    recomp_runtime.registers.ecx = 0u;

    if ((recomp_runtime.registers.eax & 0xffu) == 0u) {
        const uint8_t script = guest_u8(WORLD_SCRIPT);
        recomp_runtime.registers.eax = script;
        if (script != 0u) {
            recomp_runtime.registers.ecx = guest_u8(0x005bb9d6u);
            *recomp_memory(0x005bb9d7u, 1u) =
                guest_u8(0x005bb9d6u);
            *recomp_memory(0x005bb9d6u, 1u) = script;
            *recomp_memory(0x005bb9d8u, 1u) = 1u;
            *recomp_memory_u16(0x005bb9d4u) = 0u;
        } else {
            *recomp_memory(0x005bb9d8u, 1u) = 0u;
        }
    }

    const uint32_t matrix_stack = guest_u32(WORLD_MATRIX_STACK) - 0x40u;
    const uint32_t matrix_alignment = WORLD_MATRIX & 0x0fu;
    *recomp_memory_u32(WORLD_MATRIX_STACK) = matrix_stack;
    *recomp_memory_u32(frame - 8u) = matrix_stack;
    *recomp_memory_u32(frame - 4u) = matrix_alignment;
    recomp_runtime.registers.esi = matrix_alignment;
    recomp_runtime.registers.edi = 0x005bbb50u;
    recomp_runtime.registers.ecx = matrix_stack;
    recomp_runtime.registers.eax = WORLD_MATRIX;
    copy_matrix(matrix_stack, WORLD_MATRIX);
    recomp_runtime.registers.edx = WORLD_RENDER_MATRIX;
    recomp_runtime.registers.ecx = WORLD_MATRIX;
    recomp_runtime.registers.eax = WORLD_RENDER_MATRIX;
    copy_matrix(WORLD_MATRIX, WORLD_RENDER_MATRIX);

    push(0u);
    push(0u);
    push(0u);
    call_at_frame(frame, 0x0017b2e0u);
    recomp_runtime.registers.esp += 0x0cu;

    recomp_runtime.registers.ecx = 0x005bb980u;
    call_at_frame(frame, 0x0017b320u);

    recomp_runtime.fpu_top = (recomp_runtime.fpu_top + 7u) & 7u;
    const uint32_t angle_slot = recomp_runtime.fpu_top;
    recomp_runtime.fpu_stack[angle_slot] =
        (double)(int32_t)guest_u32(0x005bb97cu);
    recomp_runtime.fpu_stack[angle_slot] *= guest_float(0x00281288u);
    const float angle = (float)recomp_runtime.fpu_stack[angle_slot];
    recomp_runtime.registers.eax = frame - 8u;
    recomp_runtime.registers.ecx = frame - 0x0cu;
    push(frame - 8u);
    push(frame - 0x0cu);
    uint32_t angle_bits;
    memcpy(&angle_bits, &angle, sizeof angle_bits);
    push(angle_bits);
    recomp_runtime.fpu_top = (recomp_runtime.fpu_top + 1u) & 7u;
    call_at_frame(frame, 0x0017ab70u);

    recomp_runtime.registers.edx = guest_u32(frame - 8u);
    recomp_runtime.registers.eax = guest_u32(frame - 0x0cu);
    push(recomp_runtime.registers.edx);
    push(recomp_runtime.registers.eax);
    push(WORLD_MATRIX);
    call_at_frame(frame, 0x0017c190u);
    recomp_runtime.registers.esp += 0x18u;

    recomp_runtime.registers.ecx = WORLD_SAVED_MATRIX;
    recomp_runtime.registers.eax = WORLD_MATRIX;
    copy_matrix(WORLD_SAVED_MATRIX, WORLD_MATRIX);

    draw_indexed_vertex_calls(frame, 0x00325cecu, 5u);

    recomp_runtime.registers.eax = WORLD_SAVED_MATRIX;
    recomp_runtime.registers.ecx = WORLD_MATRIX;
    copy_matrix(WORLD_MATRIX, WORLD_SAVED_MATRIX);
    draw_indexed_world_items(frame, 0x00325e60u, 2u, 0x0007c7c0u);
    call_at_frame(frame, 0x0007c710u);

    recomp_runtime.registers.eax = WORLD_SAVED_MATRIX;
    recomp_runtime.registers.ecx = WORLD_MATRIX;
    copy_matrix(WORLD_MATRIX, WORLD_SAVED_MATRIX);
    draw_indexed_world_items(frame, 0x00325e50u, 4u, 0x0007c7c0u);
    call_at_frame(frame, 0x0007c660u);
    call_at_frame(frame, 0x0007bbc0u);
    call_at_frame(frame, 0x0007e280u);
    call_at_frame(frame, 0x0007ce00u);

    draw_indexed_vertex_calls(frame, 0x00325cb8u, 0x1au);
    call_at_frame(frame, 0x00078d80u);
    call_at_frame(frame, 0x0007d930u);

    recomp_runtime.registers.esi = guest_u32(frame + 8u);
    push(recomp_runtime.registers.esi);
    call_at_frame(frame, 0x0007ad10u);
    recomp_runtime.registers.esp += 4u;

    recomp_runtime.registers.eax = (recomp_runtime.registers.eax & 0xffffff00u) |
        guest_u8(WORLD_PAUSE);
    if ((recomp_runtime.registers.eax & 0xffu) == 0u) {
        const uint16_t previous = guest_u16(WORLD_COUNTER);
        *recomp_memory_u16(WORLD_COUNTER) = (uint16_t)(
            previous + world_counter_increment(steps_per_tick));
    }
    call_at_frame(frame, 0x0007dc50u);

    recomp_runtime.registers.edi = guest_u32(frame - 8u);
    recomp_runtime.registers.ebx = guest_u32(frame - 4u);
    recomp_runtime.registers.ecx = WORLD_MATRIX;
    recomp_runtime.registers.eax = WORLD_SAVED_MATRIX;
    copy_matrix(WORLD_MATRIX, WORLD_SAVED_MATRIX);

    call_at_frame(frame, 0x0007e3b0u);
    call_at_frame(frame, 0x0007e770u);
    push(recomp_runtime.registers.esi);
    call_at_frame(frame, 0x0007bd30u);
    recomp_runtime.registers.esp += 4u;
    call_at_frame(frame, 0x00079970u);

    *recomp_memory_u32(WORLD_FUNCTION_A) = 0x0007a6e0u;
    *recomp_memory_u32(WORLD_FUNCTION_B) = 0x0007a960u;
    recomp_runtime.registers.ecx = WORLD_MATRIX;
    recomp_runtime.registers.eax = WORLD_SAVED_MATRIX;
    copy_matrix(WORLD_MATRIX, WORLD_SAVED_MATRIX);
    call_at_frame(frame, 0x0007cf40u);
    push(recomp_runtime.registers.esi);
    call_at_frame(frame, 0x0007aaf0u);
    recomp_runtime.registers.esp += 4u;

    const uint32_t final_matrix_stack = guest_u32(WORLD_MATRIX_STACK);
    recomp_runtime.registers.ecx = final_matrix_stack;
    recomp_runtime.registers.eax = final_matrix_stack + 0x40u;
    *recomp_memory_u32(frame - 8u) = final_matrix_stack;
    *recomp_memory_u32(WORLD_MATRIX_STACK) = final_matrix_stack + 0x40u;
    recomp_runtime.registers.eax = final_matrix_stack;
    recomp_runtime.registers.ecx = WORLD_MATRIX;
    copy_matrix(WORLD_MATRIX, final_matrix_stack);

    recomp_runtime.registers.edi = pop();
    recomp_runtime.registers.esi = pop();
    recomp_runtime.registers.ebx = pop();
    recomp_runtime.registers.esp = frame;
    *recomp_ebp_register() = pop();
    recomp_runtime.registers.esp += 4u;
}

static void world_draw_entry(void)
{
    world_draw_model(scheduler_steps_per_tick());
    if (recomp_game_timing_enabled() && ++timed_world_frames == 300) {
        const double scale = 1000.0 / recomp_xapi_performance_frequency();
        for (unsigned i = 0; i < call_time_count; ++i) {
            fprintf(stderr, "recomp world timing: child=%08x calls=%u total_ms=%.3f\n",
                call_times[i].address, call_times[i].calls, call_times[i].elapsed * scale);
        }
    }
}

/* Generated-dispatch seams: 0x17B2E0, 0x17B320, 0x17AB70, 0x17C190,
   0x17D130, 0x17D0A0, 0x7AAF0, 0x7C7C0, 0x7C710, 0x7C660, 0x7BBC0,
   0x7E280, 0x7CE00, 0x78D80, 0x7D930, 0x7DC50, 0x7E3B0, 0x7E770,
   0x7BD30, 0x79970 and 0x7CF40. Record these seams in the tracked world
   feature documentation when this private candidate is integrated. */
RecompFunction recomp_game_world_lookup_manual(uint32_t address)
{
#ifdef RECOMP_D3D_FRAME_ENABLED
    if (address == 0x001e78b0u && draw_trace_at()) return trace_draw;
#endif
    RecompFunction motion = recomp_game_world_motion_lookup_manual(address);
    if (motion == NULL) motion = recomp_game_world_wave_lookup_manual(address);
    if (motion != NULL) return motion;
    switch (address) {
    case 0x0007a1d0u: return world_draw_entry;
    case 0x0007ad10u: return world_uv_update_entry;
    default: return recomp_game_world_timers_lookup_manual(address);
    }
}
