#include "game_camera_script.h"

#include "game_scheduler.h"


enum {
    CAMERA_BASE = 0x0041a800u,
    CAMERA_STRIDE = 0x00000ae0u,
    CAMERA_COUNT = 5u,
    IDENTITY_MATRIX = 0x00287130u,
    MATRIX_SCRATCH = 0x00a24190u,
    MATRIX_TOP = 0x00a241d4u,
    SCRIPT_COMMANDS = 0x00344680u,
    VECTOR_COMMANDS = 0x003446c0u,
    SCALAR_COMMANDS = 0x003446e8u,
};

enum {
    CAMERA_SCRIPT_AGE = 0x954u,
    CAMERA_SCRIPT_WAIT = 0x980u,
    CAMERA_SCRIPT_WAIT_SUBTICK = 0x984u,
    CAMERA_SCRIPT_COMMAND = 0x988u,
    CAMERA_SCRIPT_PASSES = 0xac9u,
    CAMERA_SCRIPT_RESET = 0xacau,
    CAMERA_SCRIPT_ID = 0xacdu,
    CAMERA_SCRIPT_SLOT = 0xad9u,
};

enum {
    VECTOR_TYPE = 0x6cu,
    VECTOR_PREVIOUS_TYPE = 0x70u,
    VECTOR_SOURCE = 0x54u,
    VECTOR_MATRIX = 0x50u,
    VECTOR_FALLBACK = 0x90u,
    VECTOR_AGE = 0x7cu,
    VECTOR_DURATION = 0x78u,
    VECTOR_SUBTICK = 0x80u,
    VECTOR_WAIT = 0x84u,
    VECTOR_WAIT_SUBTICK = 0x88u,
    VECTOR_OUTPUT = 0x40u,
};

enum {
    SCALAR_TYPE = 0x34u,
    SCALAR_SOURCE = 0x1cu,
    SCALAR_TARGET = 0x20u,
    SCALAR_VALUE = 0x18u,
    SCALAR_AGE = 0x40u,
    SCALAR_DURATION = 0x3cu,
    SCALAR_SUBTICK = 0x44u,
    SCALAR_WAIT = 0x48u,
    SCALAR_WAIT_SUBTICK = 0x4cu,
};

static unsigned script_age_subtick[CAMERA_COUNT];

static uint8_t guest_byte(uint32_t address)
{
    return *recomp_memory(address, 1u);
}

static uint16_t guest_half(uint32_t address)
{
    return *recomp_memory_u16(address);
}

static uint32_t guest_word(uint32_t address)
{
    return *recomp_memory_u32(address);
}

static int32_t guest_sword(uint32_t address)
{
    return (int32_t)guest_word(address);
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

static uint32_t float_word(float value)
{
    union {
        float f;
        uint32_t u;
    } bits = { value };
    return bits.u;
}

static void push_guest(uint32_t value)
{
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = value;
}

/* args are cdecl order; every child inherits the owning guest frame. */
static uint32_t call_guest(
    uint32_t frame,
    uint32_t address,
    const uint32_t *args,
    size_t arg_count)
{
    RecompRegisters *registers = &recomp_runtime.registers;
    const uint32_t saved_esp = registers->esp;

    for (size_t i = arg_count; i > 0u; --i) push_guest(args[i - 1u]);
    push_guest(0u);
    registers->ebp = frame;
    recomp_dispatch_indirect_site(address, saved_esp, __FILE__, __LINE__);
    const uint32_t result = registers->eax;
    registers->esp = saved_esp;
    registers->ebp = frame;
    return result;
}

static unsigned camera_steps_per_tick(void)
{
    const unsigned hz = recomp_game_scheduler_hz();
    return hz >= 60u && hz % 60u == 0u ? hz / 60u : 1u;
}

static int camera_slot(uint32_t camera)
{
    if (camera < CAMERA_BASE) return -1;
    const uint32_t offset = camera - CAMERA_BASE;
    if (offset % CAMERA_STRIDE != 0u) return -1;
    const uint32_t slot = offset / CAMERA_STRIDE;
    return slot < CAMERA_COUNT ? (int)slot : -1;
}

static uint32_t matrix_push(void)
{
    const uint32_t saved_top = guest_word(MATRIX_TOP);
    const uint32_t saved_matrix = saved_top - 0x40u;
    recomp_guest_memcpy(saved_matrix, MATRIX_SCRATCH, 0x40u);
    *recomp_memory_u32(MATRIX_TOP) = saved_matrix;
    return saved_matrix;
}

static void matrix_pop(uint32_t frame, uint32_t saved_matrix)
{
    *recomp_memory_u32(MATRIX_TOP) = saved_matrix + 0x40u;
    recomp_guest_memcpy(MATRIX_SCRATCH, saved_matrix, 0x40u);
    recomp_runtime.registers.ebp = frame;
}

static void matrix_concat(uint32_t frame)
{
    recomp_runtime.registers.eax = MATRIX_SCRATCH;
    (void)call_guest(frame, 0x0017bec0u, NULL, 0u);
}

static void transform_vector(uint32_t frame, uint32_t destination, uint32_t source)
{
    recomp_runtime.registers.edx = destination;
    recomp_runtime.registers.ecx = source;
    (void)call_guest(frame, 0x0017b3f0u, NULL, 0u);
}

static double curve_factor(
    uint32_t frame,
    uint32_t age_address,
    uint32_t duration_address,
    uint32_t subtick_address,
    unsigned steps_per_tick)
{
    const uint32_t args[] = {
        guest_word(age_address),
        guest_word(duration_address),
        guest_word(subtick_address),
        steps_per_tick,
    };
    (void)call_guest(frame, 0x000283f0u, args, 4u);

    const uint32_t top = recomp_runtime.fpu_top & 7u;
    const double value = recomp_runtime.fpu_stack[top];
    recomp_runtime.fpu_top = (top + 1u) & 7u;
    return value;
}

static void start_scaled_wait(
    uint32_t context,
    uint32_t wait_offset,
    uint32_t subtick_offset,
    int32_t wait_before,
    unsigned steps_per_tick)
{
    const int32_t wait_after = guest_sword(context + wait_offset);
    if (wait_before <= 0 && wait_after > 0 &&
        guest_word(context + subtick_offset) == 1u) {
        *recomp_memory_u32(context + subtick_offset) = steps_per_tick;
    }
}

static void consume_curve_commands(
    uint32_t context,
    uint32_t frame,
    uint32_t command_offset,
    uint32_t table,
    uint32_t wait_offset,
    uint32_t subtick_offset,
    bool context_in_edi,
    unsigned steps_per_tick)
{
    RecompRegisters *registers = &recomp_runtime.registers;

    for (;;) {
        const uint32_t command_list = guest_word(context + command_offset);
        if (command_list == 0u) break;
        const uint16_t command = guest_half(command_list);
        if (command >= 10u || guest_sword(context + wait_offset) >= 1) break;

        const int32_t wait_before = guest_sword(context + wait_offset);
        const uint32_t target = guest_word(table + command * 4u);
        const uint32_t arg = context;
        registers->eax = command;
        if (context_in_edi) registers->edi = context;
        else registers->esi = context;
        (void)call_guest(frame, target, &arg, 1u);
        if (wait_before <= 0 && guest_sword(context + wait_offset) >= 0 &&
            guest_word(context + subtick_offset) == 1u) {
            *recomp_memory_u32(context + subtick_offset) = steps_per_tick;
        }
    }
}

static void advance_scaled_wait(
    uint32_t context,
    uint32_t wait_offset,
    uint32_t subtick_offset,
    unsigned steps_per_tick)
{
    if (guest_sword(context + wait_offset) <= 0) return;

    const int32_t subtick = guest_sword(context + subtick_offset) - 1;
    *recomp_memory_u32(context + subtick_offset) = (uint32_t)subtick;
    if (subtick < 1) {
        *recomp_memory_u32(context + wait_offset) =
            guest_word(context + wait_offset) - 1u;
        *recomp_memory_u32(context + subtick_offset) = steps_per_tick;
    }
}

static void advance_curve_age(
    uint32_t context,
    uint32_t age_offset,
    uint32_t duration_offset,
    uint32_t subtick_offset,
    uint32_t type_offset,
    uint32_t previous_type_offset,
    unsigned steps_per_tick)
{
    int32_t age = guest_sword(context + age_offset);
    const int32_t duration = guest_sword(context + duration_offset);
    int32_t subtick = guest_sword(context + subtick_offset);

    if (age <= duration) {
        ++subtick;
        if (subtick >= (int32_t)steps_per_tick) {
            ++age;
            subtick = 0;
        }
        *recomp_memory_u32(context + age_offset) = (uint32_t)age;
        *recomp_memory_u32(context + subtick_offset) = (uint32_t)subtick;
    }

    if (age > duration || (age == duration && subtick > 0)) {
        if (previous_type_offset != 0u) {
            *recomp_memory_u32(context + previous_type_offset) =
                guest_word(context + type_offset);
        }
        *recomp_memory_u32(context + type_offset) = 0u;
    }
}

static void transform_active_curve(
    uint32_t frame,
    uint32_t camera,
    uint32_t context)
{
    const uint32_t saved_matrix = matrix_push();
    const uint32_t matrix = guest_word(context + VECTOR_MATRIX);
    recomp_guest_memcpy(
        MATRIX_SCRATCH,
        matrix != 0u ? matrix : IDENTITY_MATRIX,
        0x40u);
    transform_vector(frame, frame - 0x14u, context + VECTOR_OUTPUT);
    recomp_guest_memcpy(
        guest_word(context + 0x64u), frame - 0x14u, 12u);
    matrix_pop(frame, saved_matrix);
}

static void transform_fallback_curve(
    uint32_t frame,
    uint32_t camera,
    uint32_t context)
{
    const uint32_t saved_matrix = matrix_push();
    const uint32_t output = guest_word(context + 0x64u);
    const uint32_t intermediate = frame - 0x20u;

    recomp_guest_memcpy(MATRIX_SCRATCH, camera + 0x910u, 0x40u);
    matrix_concat(frame);
    transform_vector(frame, intermediate, output);
    recomp_guest_memcpy(MATRIX_SCRATCH, camera + 0x7d0u, 0x40u);
    transform_vector(frame, frame - 0x14u, intermediate);
    recomp_guest_memcpy(output, frame - 0x14u, 12u);
    matrix_pop(frame, saved_matrix);
}

static int vector_curve_step(
    uint32_t context,
    uint32_t frame,
    unsigned steps_per_tick)
{
    RecompRegisters *registers = &recomp_runtime.registers;
    const uint32_t saved_ebx = registers->ebx;
    const uint32_t saved_esi = registers->esi;
    const uint32_t saved_edi = registers->edi;
    int result = 0;
    const int32_t wait_before = guest_sword(context + VECTOR_WAIT);

    registers->edx = context;
    (void)call_guest(frame, 0x00028250u, NULL, 0u);
    start_scaled_wait(
        context, VECTOR_WAIT, VECTOR_WAIT_SUBTICK,
        wait_before, steps_per_tick);
    consume_curve_commands(
        context, frame, 0x94u, VECTOR_COMMANDS,
        VECTOR_WAIT, VECTOR_WAIT_SUBTICK, true, steps_per_tick);

    const uint32_t type = guest_word(context + VECTOR_TYPE);
    if (type == 1u) {
        recomp_guest_memcpy(
            context + VECTOR_OUTPUT,
            guest_word(context + VECTOR_SOURCE),
            12u);
        result = 1;
    } else if (type >= 2u && type <= 4u) {
        registers->ebx = guest_word(context + 0x74u);
        const double factor = curve_factor(
            frame,
            context + VECTOR_AGE,
            context + VECTOR_DURATION,
            context + VECTOR_SUBTICK,
            steps_per_tick);
        const uint32_t arg = float_word((float)factor);
        registers->eax = context;
        if (type == 2u) {
            registers->ecx = context + VECTOR_OUTPUT;
            (void)call_guest(frame, 0x000284d0u, &arg, 1u);
        } else {
            registers->esi = context + VECTOR_OUTPUT;
            (void)call_guest(
                frame, type == 3u ? 0x00028520u : 0x000285c0u,
                &arg, 1u);
        }
        result = 1;
    } else if (type == 5u) {
        const uint32_t matrix = guest_word(context + VECTOR_MATRIX);
        if (matrix == 0u) {
            recomp_guest_memcpy(
                context + VECTOR_OUTPUT,
                guest_word(context + VECTOR_FALLBACK),
                12u);
        } else {
            const uint32_t saved_matrix = matrix_push();
            recomp_guest_memcpy(MATRIX_SCRATCH, matrix, 0x40u);
            matrix_concat(frame);
            transform_vector(
                frame, context + VECTOR_OUTPUT,
                guest_word(context + VECTOR_FALLBACK));
            const uint32_t parent_camera = guest_word(context + 0x98u);
            if (guest_byte(parent_camera + 0x98cu) != 0u) {
                store_float(
                    context + VECTOR_OUTPUT,
                    -guest_float(context + VECTOR_OUTPUT));
            }
            registers->eax = 1u;
            (void)call_guest(frame, 0x0017b270u, NULL, 0u);
            matrix_pop(frame, saved_matrix);
        }
        result = 1;
    }

    if (result != 0) {
        *recomp_memory_u32(context + 0x4cu) = 0x3f800000u;
    }
    advance_scaled_wait(
        context, VECTOR_WAIT, VECTOR_WAIT_SUBTICK, steps_per_tick);
    advance_curve_age(
        context,
        VECTOR_AGE,
        VECTOR_DURATION,
        VECTOR_SUBTICK,
        VECTOR_TYPE,
        VECTOR_PREVIOUS_TYPE,
        steps_per_tick);

    registers->ebx = saved_ebx;
    registers->esi = saved_esi;
    registers->edi = saved_edi;
    registers->ebp = frame;
    return result;
}

static int scalar_curve_step(
    uint32_t context,
    uint32_t frame,
    unsigned steps_per_tick)
{
    RecompRegisters *registers = &recomp_runtime.registers;
    const uint32_t saved_ebx = registers->ebx;
    const uint32_t saved_esi = registers->esi;
    const uint32_t saved_edi = registers->edi;
    int result = 0;
    const int32_t wait_before = guest_sword(context + SCALAR_WAIT);

    registers->edx = context;
    (void)call_guest(frame, 0x000282f0u, NULL, 0u);
    start_scaled_wait(
        context, SCALAR_WAIT, SCALAR_WAIT_SUBTICK,
        wait_before, steps_per_tick);
    consume_curve_commands(
        context, frame, 0x54u, SCALAR_COMMANDS,
        SCALAR_WAIT, SCALAR_WAIT_SUBTICK, false, steps_per_tick);

    const uint32_t type = guest_word(context + SCALAR_TYPE);
    if (type == 1u) {
        store_float(
            context + SCALAR_VALUE,
            guest_float(guest_word(context + SCALAR_SOURCE)));
        result = 1;
    } else if (type == 2u) {
        registers->ebx = guest_word(context + 0x38u);
        const double factor = curve_factor(
            frame,
            context + SCALAR_AGE,
            context + SCALAR_DURATION,
            context + SCALAR_SUBTICK,
            steps_per_tick);
        const float source = guest_float(guest_word(context + SCALAR_SOURCE));
        const float target = guest_float(guest_word(context + SCALAR_TARGET));
        const double difference = (double)target - (double)source;
        const double scaled = difference * factor;
        const double value = scaled + (double)source;
        store_float(context + SCALAR_VALUE, (float)value);
        result = 1;
    } else if (type == 3u) {
        registers->ebx = guest_word(context + 0x38u);
        const double factor = curve_factor(
            frame,
            context + SCALAR_AGE,
            context + SCALAR_DURATION,
            context + SCALAR_SUBTICK,
            steps_per_tick);
        const uint32_t args[] = {
            context + SCALAR_VALUE,
            float_word((float)factor),
        };
        registers->eax = context;
        (void)call_guest(frame, 0x00028650u, args, 2u);
        result = 1;
    }

    advance_scaled_wait(
        context, SCALAR_WAIT, SCALAR_WAIT_SUBTICK, steps_per_tick);
    advance_curve_age(
        context,
        SCALAR_AGE,
        SCALAR_DURATION,
        SCALAR_SUBTICK,
        SCALAR_TYPE,
        0u,
        steps_per_tick);

    registers->ebx = saved_ebx;
    registers->esi = saved_esi;
    registers->edi = saved_edi;
    registers->ebp = frame;
    return result;
}

static void camera_interpolation_body(
    uint32_t entry_esp,
    unsigned steps_per_tick)
{
    RecompRegisters *registers = &recomp_runtime.registers;
    const uint32_t inherited_ebp = registers->ebp;
    const uint32_t saved_ebx = registers->ebx;
    const uint32_t saved_esi = registers->esi;
    const uint32_t saved_edi = registers->edi;

    push_guest(inherited_ebp);
    const uint32_t frame = registers->esp;
    registers->ebp = frame;
    registers->esp -= 0x20u;
    push_guest(saved_ebx);
    push_guest(saved_esi);
    push_guest(saved_edi);

    const uint32_t camera = guest_word(frame + 8u);
    const uint32_t first_vector = camera + 0x5e0u;
    const uint32_t second_vector = camera + 0x67cu;
    const uint32_t first_scalar = camera + 0x718u;
    const uint32_t second_scalar = camera + 0x774u;

    *recomp_memory_u32(first_vector + 0x64u) = camera + 0x120u;
    *recomp_memory_u32(first_vector + 0x68u) = camera + 0x10u;
    recomp_guest_memcpy(first_vector + VECTOR_OUTPUT, camera + 0x120u, 12u);
    if (vector_curve_step(first_vector, frame, steps_per_tick) == 0) {
        if (guest_byte(camera + 0x98fu) != 0u) {
            transform_fallback_curve(frame, camera, first_vector);
        }
    } else {
        if (guest_byte(camera + 0x98cu) == 1u) {
            store_float(
                first_vector + VECTOR_OUTPUT,
                -guest_float(first_vector + VECTOR_OUTPUT));
        }
        transform_active_curve(frame, camera, first_vector);
    }

    *recomp_memory_u32(second_vector + 0x64u) = camera + 0x110u;
    *recomp_memory_u32(second_vector + 0x68u) = camera;
    recomp_guest_memcpy(second_vector + VECTOR_OUTPUT, camera + 0x110u, 12u);
    if (vector_curve_step(second_vector, frame, steps_per_tick) == 0) {
        if (guest_byte(camera + 0x98fu) != 0u) {
            transform_fallback_curve(frame, camera, second_vector);
        }
    } else {
        if (guest_byte(camera + 0x98cu) == 1u) {
            store_float(
                second_vector + VECTOR_OUTPUT,
                -guest_float(second_vector + VECTOR_OUTPUT));
        }
        transform_active_curve(frame, camera, second_vector);
    }

    const double radians =
        (double)guest_float(camera + 0x144u) *
        (double)guest_float(0x00281798u);
    *recomp_memory_u32(first_scalar + 0x2cu) = first_scalar + 0x10u;
    store_float(first_scalar + 0x10u, (float)radians);
    *recomp_memory_u32(first_scalar + 0x30u) = first_scalar + 0x14u;
    store_float(
        first_scalar + 0x14u,
        (float)((double)guest_float(camera + 0x34u) *
                (double)guest_float(0x00281798u)));
    *recomp_memory_u32(first_scalar + SCALAR_VALUE) =
        guest_word(first_scalar + 0x10u);
    if (scalar_curve_step(first_scalar, frame, steps_per_tick) != 0) {
        const uint32_t output = guest_word(first_scalar + 0x2cu);
        if (guest_byte(camera + 0x98cu) == 1u) {
            store_float(output, -guest_float(first_scalar + SCALAR_VALUE));
        } else {
            *recomp_memory_u32(output) =
                guest_word(first_scalar + SCALAR_VALUE);
        }
        store_float(
            camera + 0x144u,
            (float)((double)guest_float(output) *
                    (double)guest_float(0x0028175cu)));
    }

    const double pitch =
        (double)guest_float(camera + 0x140u) *
        (double)guest_float(0x00281798u);
    *recomp_memory_u32(second_scalar + 0x2cu) = second_scalar + 0x10u;
    store_float(second_scalar + 0x10u, (float)pitch);
    *recomp_memory_u32(second_scalar + 0x30u) = second_scalar + 0x14u;
    store_float(
        second_scalar + 0x14u,
        (float)((double)guest_float(camera + 0x30u) *
                (double)guest_float(0x00281798u)));
    *recomp_memory_u32(second_scalar + SCALAR_VALUE) =
        guest_word(second_scalar + 0x10u);
    (void)scalar_curve_step(second_scalar, frame, steps_per_tick);
    *recomp_memory_u32(guest_word(second_scalar + 0x2cu)) =
        guest_word(second_scalar + SCALAR_VALUE);
    store_float(
        camera + 0x140u,
        (float)((double)guest_float(guest_word(second_scalar + 0x2cu)) *
                (double)guest_float(0x0028175cu)));

    registers->ebx = saved_ebx;
    registers->esi = saved_esi;
    registers->edi = saved_edi;
    registers->ebp = inherited_ebp;
    registers->esp = entry_esp + 4u;
    registers->eax = 1u;
}

static void call_camera_interpolation(
    uint32_t camera,
    uint32_t inherited_frame,
    unsigned steps_per_tick)
{
    push_guest(camera);
    push_guest(0u);
    const uint32_t entry_esp = recomp_runtime.registers.esp;
    recomp_runtime.registers.ebp = inherited_frame;
    camera_interpolation_body(entry_esp, steps_per_tick);
    recomp_runtime.registers.esp += 4u;
}

static void handoff_camera_event(uint32_t camera)
{
    const unsigned slot = guest_byte(camera + CAMERA_SCRIPT_SLOT);
    if (slot >= CAMERA_COUNT) return;

    const uint32_t destination = CAMERA_BASE + slot * CAMERA_STRIDE;
    *recomp_memory(destination + 0x420u, 1u) =
        guest_byte(camera + CAMERA_SCRIPT_ID);
    *recomp_memory(destination + 0x421u, 1u) = 1u;
    recomp_guest_memset(destination + 0x3f8u, 0, 10u * sizeof(uint32_t));
    *recomp_memory_u16(destination + 0x3fau) = 0u;
}

static void initialize_camera_script(
    uint32_t camera,
    uint32_t frame,
    unsigned steps_per_tick)
{
    RecompRegisters *registers = &recomp_runtime.registers;
    registers->eax = camera;
    (void)call_guest(frame, 0x0002da70u, NULL, 0u);

    *recomp_memory(camera + 0xad5u, 1u) = 1u;
    *recomp_memory(camera + 0xad4u, 1u) = 1u;
    *recomp_memory(camera + 0xacfu, 1u) = 1u;
    *recomp_memory(camera + 0xad2u, 1u) = 0u;
    *recomp_memory(camera + 0xad3u, 1u) = 0u;
    *recomp_memory(camera + 0x424u, 1u) = 0u;
    *recomp_memory(camera + 0x425u, 1u) = 1u;
    for (unsigned i = 0; i < 10u; ++i) {
        *recomp_memory_u32(camera + 0x958u + i * 4u) = UINT32_MAX;
    }
    *recomp_memory_u32(camera + CAMERA_SCRIPT_AGE) = 0u;
    *recomp_memory_u32(camera + CAMERA_SCRIPT_WAIT) = UINT32_MAX;
    *recomp_memory_u32(camera + CAMERA_SCRIPT_WAIT_SUBTICK) = steps_per_tick;
    *recomp_memory_u32(camera + 0x674u) = 0u;
    *recomp_memory_u32(camera + 0x66cu) = 0u;
    *recomp_memory_u32(camera + 0x664u) = UINT32_MAX;
    *recomp_memory_u32(camera + 0x64cu) = 0u;
    *recomp_memory_u32(camera + 0x710u) = 0u;
    *recomp_memory_u32(camera + 0x708u) = 0u;
    *recomp_memory_u32(camera + 0x700u) = UINT32_MAX;
    *recomp_memory_u32(camera + 0x6e8u) = 0u;
    *recomp_memory_u32(camera + 0x650u) = 0u;
    *recomp_memory_u32(camera + 0x6ecu) = 0u;
    *recomp_memory_u32(camera + 0x76cu) = 0u;
    *recomp_memory_u32(camera + 0x768u) = 0u;
    *recomp_memory_u32(camera + 0x760u) = UINT32_MAX;
    *recomp_memory_u32(camera + 0x74cu) = 0u;
    *recomp_memory_u32(camera + 0x7c8u) = 0u;
    *recomp_memory_u32(camera + 0x7c4u) = 0u;
    *recomp_memory_u32(camera + 0x7bcu) = UINT32_MAX;
    *recomp_memory_u32(camera + 0x7a8u) = 0u;
    *recomp_memory(camera + 0x990u, 1u) = 0xffu;
    if (guest_byte(camera + CAMERA_SCRIPT_ID) == 9u) {
        *recomp_memory_u32(camera + 0x140u) = 0x3f1e9d93u;
        *recomp_memory_u32(camera + 0x144u) = 0u;
    }

    const int slot = camera_slot(camera);
    if (slot >= 0) script_age_subtick[slot] = 0u;
}

static void run_camera_script(
    uint32_t camera,
    uint32_t frame,
    unsigned steps_per_tick)
{
    RecompRegisters *registers = &recomp_runtime.registers;
    const int slot = camera_slot(camera);
    unsigned age_subtick = slot >= 0 ? script_age_subtick[slot] : 0u;
    bool overflow = false;
    registers->ebx = 0u;

    if (guest_byte(camera + CAMERA_SCRIPT_RESET) != 0u) {
        initialize_camera_script(camera, frame, steps_per_tick);
        age_subtick = 0u;
    }

    registers->eax = camera;
    (void)call_guest(frame, 0x000263c0u, NULL, 0u);
    *recomp_memory_u32(frame + 8u) = 0u;
    const uint8_t passes = guest_byte(camera + CAMERA_SCRIPT_PASSES);
    for (uint32_t pass = 0; pass < passes; ++pass) {
        unsigned consumed = 0u;
        while (consumed < 0x200u) {
            const uint32_t command_list = guest_word(camera + CAMERA_SCRIPT_COMMAND);
            if (command_list == 0u) break;
            const uint16_t command = guest_half(command_list);
            if (command >= 16u || guest_sword(camera + CAMERA_SCRIPT_WAIT) > 0) {
                break;
            }

            const int32_t wait_before = guest_sword(camera + CAMERA_SCRIPT_WAIT);
            const uint32_t target = guest_word(SCRIPT_COMMANDS + command * 4u);
            const uint32_t arg = camera;
            registers->eax = command;
            registers->esi = camera;
            (void)call_guest(frame, target, &arg, 1u);
            ++consumed;
            if (target == 0x00027160u) {
                *recomp_memory_u32(camera + CAMERA_SCRIPT_WAIT_SUBTICK) =
                    steps_per_tick;
            } else if (wait_before <= 0 &&
                       guest_sword(camera + CAMERA_SCRIPT_WAIT) > 0 &&
                       guest_word(camera + CAMERA_SCRIPT_WAIT_SUBTICK) == 1u) {
                *recomp_memory_u32(camera + CAMERA_SCRIPT_WAIT_SUBTICK) =
                    steps_per_tick;
            }
        }

        if (guest_sword(camera + CAMERA_SCRIPT_WAIT) >= 0) {
            call_camera_interpolation(camera, frame, steps_per_tick);
            const int32_t subtick =
                guest_sword(camera + CAMERA_SCRIPT_WAIT_SUBTICK) - 1;
            *recomp_memory_u32(camera + CAMERA_SCRIPT_WAIT_SUBTICK) =
                (uint32_t)subtick;
            if (subtick <= 0) {
                *recomp_memory_u32(camera + CAMERA_SCRIPT_WAIT) =
                    guest_word(camera + CAMERA_SCRIPT_WAIT) - 1u;
                *recomp_memory_u32(camera + CAMERA_SCRIPT_WAIT_SUBTICK) =
                    steps_per_tick;
            }
        }

        if (consumed >= 0x200u) {
            overflow = true;
            break;
        }

        if (slot >= 0) {
            if (++age_subtick >= steps_per_tick) {
                age_subtick = 0u;
                *recomp_memory_u32(camera + CAMERA_SCRIPT_AGE) += 1u;
            }
        } else if (steps_per_tick == 1u) {
            *recomp_memory_u32(camera + CAMERA_SCRIPT_AGE) += 1u;
        }
        *recomp_memory_u32(frame + 8u) = pass + 1u;
    }

    if (slot >= 0) script_age_subtick[slot] = age_subtick;
    if (guest_sword(camera + CAMERA_SCRIPT_WAIT) < 0) {
        handoff_camera_event(camera);
    }
    if (overflow) handoff_camera_event(camera);
}

static void camera_script_callback(void)
{
    RecompRegisters *registers = &recomp_runtime.registers;
    const uint32_t entry_esp = registers->esp;
    const uint32_t inherited_ebp = registers->ebp;
    const uint32_t saved_ecx = registers->ecx;
    const uint32_t saved_ebx = registers->ebx;
    const uint32_t saved_esi = registers->esi;
    const uint32_t saved_edi = registers->edi;

    push_guest(inherited_ebp);
    const uint32_t frame = registers->esp;
    registers->ebp = frame;
    push_guest(saved_ecx);
    push_guest(saved_ebx);
    push_guest(saved_esi);
    push_guest(saved_edi);

    const uint32_t camera = guest_word(frame + 8u);
    registers->esi = camera;
    run_camera_script(camera, frame, camera_steps_per_tick());

    registers->ebx = saved_ebx;
    registers->esi = saved_esi;
    registers->edi = saved_edi;
    registers->ebp = inherited_ebp;
    registers->esp = entry_esp + 4u;
}

static void camera_interpolation_callback(void)
{
    camera_interpolation_body(
        recomp_runtime.registers.esp,
        camera_steps_per_tick());
}

static void vector_curve_callback(void)
{
    RecompRegisters *registers = &recomp_runtime.registers;
    const uint32_t entry_esp = registers->esp;
    const uint32_t frame = registers->ebp;
    const uint32_t context = registers->edi;
    const uint32_t saved_ebx = registers->ebx;
    const uint32_t saved_esi = registers->esi;
    const uint32_t saved_edi = registers->edi;
    const int result = vector_curve_step(
        context, frame, camera_steps_per_tick());

    registers->ebx = saved_ebx;
    registers->esi = saved_esi;
    registers->edi = saved_edi;
    registers->ebp = frame;
    registers->esp = entry_esp + 4u;
    registers->eax = (uint32_t)result;
}

static void scalar_curve_callback(void)
{
    RecompRegisters *registers = &recomp_runtime.registers;
    const uint32_t entry_esp = registers->esp;
    const uint32_t frame = registers->ebp;
    const uint32_t context = registers->esi;
    const uint32_t saved_ebx = registers->ebx;
    const uint32_t saved_esi = registers->esi;
    const uint32_t saved_edi = registers->edi;
    const int result = scalar_curve_step(
        context, frame, camera_steps_per_tick());

    registers->ebx = saved_ebx;
    registers->esi = saved_esi;
    registers->edi = saved_edi;
    registers->ebp = frame;
    registers->esp = entry_esp + 4u;
    registers->eax = (uint32_t)result;
}

RecompFunction recomp_game_camera_script_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x00026160u: return camera_script_callback;
    case 0x000271d0u: return camera_interpolation_callback;
    case 0x00027570u: return vector_curve_callback;
    case 0x00027f40u: return scalar_curve_callback;
    default: return NULL;
    }
}
