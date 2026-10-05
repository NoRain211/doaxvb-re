#include "game_secondary.h"
#include "game_scheduler.h"

#include <math.h>
#include <stdio.h>

/* The tick model has no presentation state. At higher rates the whole
   secondary cluster (chains, deformation and morph helpers) advances once per
   authored tick; its generated helpers use per-tick constants, so stepping one
   member fractionally would desynchronize it. */
static void secondary_particle(float state[16], const float root[4],
    const float carry[4], const float force[4], float coupling, float damping,
    float gravity)
{
    for (unsigned axis = 0; axis < 4; ++axis) {
        const float displacement = state[axis] - root[axis];
        float velocity = state[8 + axis] + displacement;
        velocity -= state[12 + axis];
        velocity *= damping;
        velocity -= axis == 1 ? gravity : 0;
        float advance = displacement * coupling;
        advance += velocity;
        advance += carry[axis];
        advance += force[axis];
        state[4 + axis] += advance;
        state[8 + axis] = velocity;
        state[12 + axis] = displacement;
    }
}

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
static uint32_t word(uint32_t address) { return *recomp_memory_u32(address); }
static uint8_t byte(uint32_t address) { return *recomp_memory(address, 1); }
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

static void constrain_length(uint32_t vector, float limit)
{
    const double x = scalar(vector), y = scalar(vector + 4), z = scalar(vector + 8);
    const double squared = x * x + y * y + z * z;
    if (squared == scalar(0x0028124cu)) return;
    const float rounded = (float)squared;
    const float reciprocal = 1.0f / sqrtf(rounded);
    float correction = rounded * reciprocal;
    correction *= reciprocal;
    correction *= reciprocal;
    correction *= scalar(0x002403d0u);
    float distance = reciprocal * scalar(0x002403f0u);
    distance -= correction;
    distance *= rounded;
    if (distance > scalar(0x0028124cu)) {
        const double factor = (double)limit / distance;
        store(vector, (float)(x * factor));
        store(vector + 4, (float)(y * factor));
        store(vector + 8, (float)(z * factor));
    }
}

static void secondary_chain(void)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t caller_frame = *recomp_ebp_register();
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t record = word(stack + 4);
    const unsigned selector = byte(record + 1);
    push(caller_frame);
    const uint32_t frame = recomp_runtime.registers.esp;
    recomp_runtime.registers.esp -= 0x4c;
    push(saved_edi);
    recomp_runtime.registers.edi = record;
    if ((selector == 0xff && !byte(0x00499a24u)) ||
        ((selector == 0xee || selector == 0xef) && !word(0x00499a18u))) {
        recomp_runtime.registers.edi = saved_edi;
        *recomp_ebp_register() = caller_frame;
        recomp_runtime.registers.esp = stack + 4;
        return;
    }
    const uint32_t matrix = selector == 0xff ? 0x00493130u :
        word(0x00499b48u) + (selector == 0xee || selector == 0xef ? 0 : selector * 64);
    push(matrix); push(0x00a24190u);
    call(frame, 0x0002ee60u);
    recomp_runtime.registers.esp += 8;
    const uint32_t root = frame - 44, carry_address = frame - 76;
    recomp_runtime.registers.ecx = record + 0x14;
    recomp_runtime.registers.edx = root;
    call(frame, 0x0017b3f0u);
    const uint32_t wind = 0x00a23478u + ((word(0x00a23680u) - 1) & 31u) * 16u;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const float value = byte(0x00a23689u) ? 0 : scalar(wind + axis * 4);
        store(0x00499b38u + axis * 4, (float)((double)value * scalar(record + 0x3c)));
    }
    recomp_runtime.registers.ecx = record + 0x24;
    recomp_runtime.registers.edx = carry_address;
    call(frame, 0x0017b430u);
    push(word(root + 8)); push(word(root));
    call(frame, 0x00121490u);
    const float floor_height = (float)(recomp_runtime.fpu_stack[recomp_runtime.fpu_top] + scalar(0x00281514u));
    recomp_runtime.fpu_top = (recomp_runtime.fpu_top + 1u) & 7u;
    recomp_runtime.registers.esp += 8;
    float damping = scalar(record + 0x10);
    float gravity = (float)((double)scalar(record + 8) * scalar(0x0028128cu));
    float carry_decay = scalar(record + 0x34);
    const float segment_length = scalar(record + 4);
    store(frame - 28, floor_height);
    store(frame - 8, damping);
    store(frame - 20, segment_length);
    *recomp_memory_u32(frame - 16) = word(record + 0x64);
    store(frame - 56, gravity);
    store(frame - 60, 0); store(frame - 52, 0); store(frame - 48, 0);
    store(frame - 4, carry_decay);
    call(frame, 0x0005fe60u);
    if (recomp_runtime.registers.eax) {
        if (word(record + 8) == 0x3c9374bcu ||
            (word(record + 8) == 0x3bc49ba6u && word(record + 0x34) == 0x3f3374bcu)) {
            carry_decay = 0.7f;
        } else carry_decay = (float)((double)carry_decay * scalar(0x0028144cu));
        double threshold;
        recomp_guest_load(&threshold, 0x00281908u, sizeof threshold);
        if (damping < threshold) damping = (float)((double)damping * scalar(0x00281604u));
        gravity = 0;
    }
    store(frame - 8, damping); store(frame - 4, carry_decay); store(frame - 56, gravity);
    *recomp_memory_u32(frame - 24) = 0;
    if (byte(record)) {
        push(saved_ebx); push(saved_esi);
        uint32_t particle = word(record + 0x64);
        for (unsigned i = 0; i < byte(record); ++i, particle += 64) {
            float state[16], origin[4], carry[4], force[4];
            recomp_guest_load(state, particle, sizeof state);
            recomp_guest_load(origin, root, sizeof origin);
            recomp_guest_load(carry, carry_address, sizeof carry);
            recomp_guest_load(force, 0x00499b38u, sizeof force);
            secondary_particle(state, origin, carry, force,
                scalar(record + 0xc), damping, gravity);
            recomp_guest_store(particle, state, sizeof state);
            for (unsigned axis = 0; axis < 4; ++axis) carry[axis] *= carry_decay;
            recomp_guest_store(carry_address, carry, sizeof carry);
            recomp_runtime.registers.ebx = particle;
            recomp_runtime.registers.esi = particle + 16;
            constrain_length(particle + 16, segment_length);
            push(particle + 16); push(root); push(word(frame - 20)); push(word(record + 0x48));
            recomp_runtime.registers.ecx = root;
            call(frame, 0x0005fee0u);
            push(word(record + 4)); push(word(frame - 28)); push(word(root + 4));
            recomp_runtime.registers.ecx = particle + 16;
            call(frame, 0x0005e850u);
            recomp_runtime.registers.esp += 28;
            recomp_guest_memcpy(particle, root, 12);
            for (unsigned axis = 0; axis < 4; ++axis) {
                store(root + axis * 4, (float)((double)scalar(root + axis * 4) + scalar(particle + 16 + axis * 4)));
            }
            *recomp_memory_u32(frame - 16) = particle + 64;
            *recomp_memory_u32(frame - 24) = i + 1;
        }
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    *recomp_ebp_register() = caller_frame;
    recomp_runtime.registers.esp = stack + 4;
}

static void one_argument(uint32_t frame, uint32_t address, uint32_t value)
{
    push(value);
    call(frame, address);
    recomp_runtime.registers.esp += 4;
}

static void morph_frame(void)
{
    static unsigned phase;
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    const uint32_t update = word(stack + 4);
    push(saved_ebx);
    recomp_runtime.registers.ebx = 0;
    push(0); call(frame, 0x000640a0u);
    *recomp_memory(0x004ca490u, 1) = byte(0x004d56efu);
    const unsigned steps = recomp_game_scheduler_hz() / 60u;
    if (update && !byte(0x004d56efu)) {
        phase = (phase + 1) % steps;
        if (phase) *recomp_memory(0x004ca490u, 1) = 1;
    }
    if (update) {
        push(saved_esi); push(saved_edi);
        for (unsigned actor = 0; actor < 4; ++actor) {
            recomp_runtime.registers.esi = actor;
            recomp_runtime.registers.edi = 0x004d6ff1u + actor * 0xbc;
            if (!byte(0x004d6fd0u + actor) || byte(recomp_runtime.registers.edi) == 10) continue;
            recomp_runtime.registers.eax = actor;
            call(frame, 0x000643b0u);
            one_argument(frame, 0x00064720u, actor);
            recomp_runtime.registers.eax = actor;
            call(frame, 0x00064af0u);
            recomp_runtime.registers.ecx = actor;
            call(frame, 0x00064d00u);
            *recomp_memory(0x004ca1c2u + actor * 4, 1) = 0;
            *recomp_memory(0x004ca1c3u + actor * 4, 1) = 0;
            recomp_runtime.registers.eax = actor;
            call(frame, 0x00064300u);
        }
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    *recomp_ebp_register() = frame;
    recomp_runtime.registers.esp = stack + 4;
}

static void actor_frame_prepare(void)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t frame = *recomp_ebp_register();
    const uint8_t mode = byte(0x004d56dau);
    if (byte(0x005e69b5u) != mode) {
        const uint8_t previous = byte(0x005e69b4u);
        *recomp_memory(0x005e69b4u, 1) = previous == 2 || mode == 0 || (mode == 1 && previous == 0);
        *recomp_memory(0x005e69b5u, 1) = mode;
    }
    unsigned scene = byte(0x004d56e4u);
    if (byte(0x004d952cu) != byte(0x005e69bbu) || scene != byte(0x005e69bcu) ||
        byte(0x004d92f0u + scene) != byte(0x005e69b8u + scene)) {
        call(frame, 0x00143510u);
        scene = byte(0x004d56e4u);
        *recomp_memory(0x005e69bcu, 1) = scene;
        *recomp_memory(0x005e69bbu, 1) = byte(0x004d952cu);
        *recomp_memory(0x005e69b8u + scene, 1) = byte(0x004d92f0u + scene);
    }
    for (unsigned actor = 0; actor < 4; ++actor) {
        if (byte(0x004d704cu + actor * 0xbc)) *recomp_memory(0x004256f8u + actor * 0x1b668, 1) = 0xff;
    }
    ++*recomp_memory_u32(0x004c9790u);
    const uint32_t rng = recomp_game_debug_rng_state();
    push(1); call(frame, 0x00064220u);
    if (rng != recomp_game_debug_rng_state()) fprintf(stderr, "recomp morph rng: before=%08x after=%08x\n", rng, recomp_game_debug_rng_state());
    recomp_runtime.registers.ecx = word(recomp_runtime.registers.esp);
    recomp_runtime.registers.esp = stack + 4;
    *recomp_ebp_register() = frame;
}

static void secondary_owner(void)
{
    static unsigned phases[4];
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t caller_frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    const uint32_t actor_argument = word(stack + 4);
    const unsigned actor = actor_argument & 0xff;
    const uint8_t saved_pause = byte(0x004d56efu);
    const unsigned steps = recomp_game_scheduler_hz() / 60u;
    bool complete = true;
    if (actor < 4 && !saved_pause) {
        phases[actor] = (phases[actor] + 1) % steps;
        complete = phases[actor] == 0;
        if (!complete) *recomp_memory(0x004d56efu, 1) = 1;
    }
    const uint32_t owner = 0x004256f8u + actor * 0x1b668u;
    push(caller_frame);
    const uint32_t frame = recomp_runtime.registers.esp;
    recomp_runtime.registers.esp -= 12;
    push(saved_ebx); push(saved_esi); push(saved_edi);
    *recomp_memory_u32(frame - 12) = actor;
    *recomp_memory_u32(0x00493170u) = 0x00b38f20u + actor * 64;
    *recomp_memory_u32(0x004c56c0u) = owner;
    const uint32_t matrix_stack = word(0x00a241d4u) - 64;
    *recomp_memory_u32(0x00a241d4u) = matrix_stack;
    push(0x00a24190u); push(matrix_stack); call(frame, 0x0002ee60u);
    recomp_runtime.registers.esp += 8;
    *recomp_memory_u32(0x00499b48u) = 0x004d3650u + actor * 0x800;
    *recomp_memory_u32(0x004256ecu) = 0x004d72f0u + actor * 0x800;
    *recomp_memory_u32(0x004256f4u) = 0x00b34a80u + actor * 0x200;
    recomp_runtime.registers.ebx = actor;
    recomp_runtime.registers.esi = owner;
    if (byte(0x004c9798u + actor)) {
        recomp_runtime.registers.esi = actor;
        one_argument(frame, 0x00056d60u, 1);
        recomp_runtime.registers.esi = owner;
        *recomp_memory(owner, 1) = 0xff;
    } else if (byte(0x004c9898u + actor)) {
        one_argument(frame, 0x0005fc90u, actor_argument);
        *recomp_memory(owner, 1) = 0xff;
    }
    if (word(0x00499a28u) == 0) {
        const uint32_t outputs[] = {0x004256f0u, 0x004256e4u, 0x00493098u, 0x004256e0u, 0x004959b8u};
        const unsigned inputs[] = {11, 12, 15, 16, 13};
        for (unsigned i = 0; i < 5; ++i) *recomp_memory_u32(outputs[i]) = byte(owner + inputs[i]);
        *recomp_memory_u32(0x00493120u) = 0;
        *recomp_memory_u32(0x00499b34u) = 0;
    }
    *recomp_memory_u32(0x00499a18u) = word(0x00499b4cu + actor * 4);
    const uint8_t mode = byte(0x004d5664u + actor);
    *recomp_memory_u32(frame - 4) = owner + 0x2a1c;
    *recomp_memory_u32(frame - 8) = owner + 0x3474;
    if (byte(owner + 10) && mode != byte(owner + 1) && mode < 4) {
        push(mode); push(owner); call(frame, 0x00056c80u);
        recomp_runtime.registers.esp += 8;
    }
    const bool seed_chains = (byte(owner) & 1) != 0;
    const uint8_t initial_flags = byte(owner);
    for (unsigned i = 0; i < byte(owner + 2); ++i) {
        const uint32_t record = owner + 0x1c + i * 0x68;
        recomp_runtime.registers.ebx = i;
        recomp_runtime.registers.edi = record;
        if (seed_chains) {
            recomp_runtime.registers.eax = record;
            one_argument(frame, 0x00058740u, 0);
            *recomp_memory(owner, 1) &= 0xfe;
        } else {
            if (!byte(0x004d56efu)) one_argument(frame, 0x00057300u, record);
            recomp_runtime.registers.esi = record;
            one_argument(frame, 0x00057250u, 0);
        }
        recomp_runtime.registers.esi = owner;
    }
    for (unsigned i = 0; i < byte(owner + 6); ++i) {
        recomp_runtime.registers.ebx = i;
        recomp_runtime.registers.edi = word(frame - 8);
        call(frame, 0x00058a50u);
        *recomp_memory_u32(frame - 8) = recomp_runtime.registers.edi + 8;
        recomp_runtime.registers.esi = owner;
    }
    const bool seed_short = (byte(owner) & 4) != 0;
    for (unsigned i = 0; i < byte(owner + 4); ++i) {
        recomp_runtime.registers.ebx = 0x80;
        recomp_runtime.registers.edi = i;
        recomp_runtime.registers.esi = word(frame - 4);
        recomp_runtime.registers.ecx = i;
        recomp_runtime.registers.edx = actor_argument & 0xffffff00u;
        one_argument(frame, seed_short ? 0x00058ba0u : 0x00058cd0u, seed_short ? 0 : actor_argument);
        *recomp_memory_u32(frame - 4) = recomp_runtime.registers.esi + 0x80;
        recomp_runtime.registers.esi = owner;
    }
    const bool seed_small = (byte(owner) & 0x20) != 0;
    const unsigned small_count = byte(owner + 8);
    for (unsigned i = 0; i < small_count; ++i) {
        recomp_runtime.registers.esi = small_count - i;
        recomp_runtime.registers.edi = owner + 0x153d4 + i * 12;
        one_argument(frame, seed_small ? 0x000597e0u : 0x00059940u, recomp_runtime.registers.edi);
    }
    recomp_runtime.registers.esi = owner;
    if (seed_small) *recomp_memory(owner, 1) &= 0xdf;
    *recomp_memory_u32(0x00b257f0u) = word(0x00b257fcu);
    if (!byte(0x004d56d8u)) one_argument(frame, 0x00059fd0u, actor_argument);
    const unsigned timed_count = byte(owner + 9);
    for (unsigned i = 0; i < timed_count; ++i) {
        recomp_runtime.registers.esi = timed_count - i;
        recomp_runtime.registers.edi = owner + 0x15494 + i * 0xa4;
        push(0); push(recomp_runtime.registers.edi); call(frame, 0x0005d9f0u);
        recomp_runtime.registers.esp += 8;
    }
    recomp_runtime.registers.esi = owner;
    recomp_runtime.registers.edi = owner + 0xd1c;
    const unsigned large_count = byte(owner + 3);
    if (byte(owner) & 2) {
        for (unsigned i = 0; i < large_count; ++i) {
            recomp_runtime.registers.esi = large_count - i;
            recomp_runtime.registers.edi = owner + 0xd1c + i * 0x3a0;
            recomp_runtime.registers.edx = 0;
            call(frame, 0x0005b150u);
        }
    } else {
        push(0); push(large_count); push(owner + 0xd1c); call(frame, 0x0005b9e0u);
        recomp_runtime.registers.esp += 12;
    }
    const uint32_t saved_matrix = word(0x00a241d4u);
    *recomp_memory_u32(0x00a241d4u) = saved_matrix + 64;
    *recomp_memory_u32(frame + 8) = saved_matrix;
    recomp_guest_load(recomp_runtime.xmm, saved_matrix, 64);
    recomp_guest_memcpy(0x00a24190u, saved_matrix, 64);
    *recomp_memory(owner, 1) = complete ? 0 : initial_flags;
    *recomp_memory(0x004d56efu, 1) = saved_pause;
    *recomp_memory(owner + 1, 1) = byte(0x004d5664u + actor);
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    *recomp_ebp_register() = caller_frame;
    recomp_runtime.registers.esp = stack + 4;
}

static void actor_render(void)
{
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t caller_frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    const uint32_t actor = word(stack + 4), flags = word(stack + 8);
    const uint32_t frame = 0x004d6ff0u + actor * 188;
    const uint32_t bank = 0x004c97a8u + actor * 44;
    const uint32_t stamp = 0x004c9750u + actor * 12;
    const bool repeated = word(stamp) == word(0x004c9790u);
    *recomp_memory_u32(stamp) = word(0x004c9790u);
    push(repeated); push(saved_ebx); push(caller_frame); push(saved_esi);
    recomp_runtime.registers.ebx = bank;
    recomp_runtime.registers.esi = actor;
    if (byte(0x004d6fd0u + actor) && byte(frame + 5) && !byte(0x00b3caf8u)) {
        *recomp_memory_u32(0x004c978cu) = byte(0x004d56efu);
        if (repeated) *recomp_memory(0x004d56efu, 1) = 1;
        *recomp_memory_u32(bank + 24) = word(stamp - 4);
        if (byte(frame + 1) == 10) *recomp_memory(0x004c9744u + actor, 1) = 0;
        else {
            recomp_runtime.registers.eax = byte(bank);
            call(frame, 0x000a3460u);
            recomp_runtime.registers.edx = recomp_runtime.registers.eax & 0xff;
            call(frame, 0x0009d0c0u);
            *recomp_memory(0x004c9744u + actor, 1) = (uint8_t)recomp_runtime.registers.eax;
        }
        if (byte(0x004c9798u + actor)) {
            recomp_runtime.registers.eax = actor;
            call(frame, 0x000631f0u);
            recomp_runtime.registers.esi = actor;
        }
        if ((byte(0x004c9798u + actor) || byte(0x004c9898u + actor)) && byte(frame + 1) != 10) {
            recomp_runtime.registers.eax = actor;
            call(frame, 0x00063290u);
        }
        push(saved_edi);
        recomp_runtime.registers.edi = 0;
        call(frame, 0x00066f90u);
        if (byte(0x004c97a0u + actor) && !byte(0x004c9798u + actor)) one_argument(frame, 0x0005a170u, actor);
        one_argument(frame, 0x00056e90u, actor);
        if (!repeated) {
            push(word(bank + 32)); push(word(bank + 28));
            recomp_runtime.registers.eax = actor;
            call(frame, 0x00067540u);
            recomp_runtime.registers.esp += 8;
        }
        if (byte(frame + 1) != 10) {
            recomp_runtime.registers.eax = actor;
            call(frame, 0x000632e0u);
        }
        if (!byte(0x00b3caf8u)) {
            recomp_runtime.registers.eax = actor;
            call(frame, 0x00063240u);
            if (flags & 8) {
                push(word(bank + 36)); push(word(bank + 28));
                recomp_runtime.registers.eax = actor;
                call(frame, 0x00067df0u);
                recomp_runtime.registers.esp += 8;
            } else {
                recomp_runtime.registers.eax = actor;
                one_argument(frame, 0x00067d30u, word(bank + 28));
            }
            if (!(flags & 5)) {
                recomp_runtime.registers.eax = actor;
                recomp_runtime.registers.ecx = word(0x004c9860u + actor * 16);
                call(frame, 0x00067060u);
            }
            recomp_runtime.registers.edi = 1;
            call(frame, 0x00066f90u);
            *recomp_memory(0x004c9798u + actor, 1) = 0;
            *recomp_memory(0x004c9898u + actor, 1) = 0;
            *recomp_memory(0x004c97a0u + actor, 1) = 0;
            *recomp_memory(0x004d56efu, 1) = byte(0x004c978cu);
        }
    }
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    recomp_runtime.registers.ecx = repeated;
    *recomp_ebp_register() = caller_frame;
    recomp_runtime.registers.esp = stack + 12;
}

/* Game-owned chain solver. Matrix, terrain and collision helpers remain
   generated seams; the timestep model has no dispatch or presentation state. */
RecompFunction recomp_game_secondary_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x00057300u: return secondary_chain;
    case 0x00056e90u: return secondary_owner;
    case 0x00063000u: return actor_render;
    case 0x00064220u: return morph_frame;
    case 0x000b4380u: return actor_frame_prepare;
    default: return NULL;
    }
}
