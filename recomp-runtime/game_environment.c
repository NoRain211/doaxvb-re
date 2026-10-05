#include "game_environment.h"
#include "game_scheduler.h"

#include <string.h>

#define MODE_OFFSET 0x38u
#define COUNT_OFFSET 0x3au
#define TYPE_OFFSET 0x34u

static const unsigned scalar_output_offsets[11] = {
    0x04u, 0x08u, 0x0cu, 0x10u, 0x14u, 0x18u,
    0x1cu, 0x20u, 0x2cu, 0x30u, 0x34u,
};
static const unsigned scalar_input_offsets[11] = {
    0x0cu, 0x10u, 0x14u, 0x18u, 0x1cu, 0x20u,
    0x24u, 0x28u, 0x00u, 0x04u, 0x08u,
};
static const unsigned angle_input_offsets[2] = {0x2cu, 0x30u};

static uint16_t read_u16(const uint8_t *p)
{
    uint16_t value;
    memcpy(&value, p, sizeof value);
    return value;
}

static void write_u16(uint8_t *p, uint16_t value)
{
    memcpy(p, &value, sizeof value);
}

static uint32_t read_u32(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof value);
    return value;
}

static void write_u32(uint8_t *p, uint32_t value)
{
    memcpy(p, &value, sizeof value);
}

static float read_float(const uint8_t *p)
{
    float value;
    memcpy(&value, p, sizeof value);
    return value;
}

static void write_float(uint8_t *p, float value)
{
    memcpy(p, &value, sizeof value);
}

static void setup(GameEnvironmentRecord *record)
{
    const uint32_t type = read_u32(record->input + TYPE_OFFSET);

    for (unsigned i = 0; i < 11; ++i) {
        write_u32(record->output + scalar_output_offsets[i],
                  read_u32(record->input + scalar_input_offsets[i]));
    }
    for (unsigned i = 0; i < 2; ++i) {
        write_u32(record->output + 0x24u + i * 4u,
                  read_u32(record->input + angle_input_offsets[i]));
    }
    write_u32(record->output, type <= 3u ? 1u : 2u);
    if (type > 10u) {
        memcpy(record->output + 0x38u, record->input + 0x10u, 0x0cu);
        memcpy(record->output + 0x44u, record->input + 0x0cu, 4u);
    }
    write_u16(record->input + MODE_OFFSET, 3u);
    write_u16(record->input + COUNT_OFFSET, 0u);
}

static int32_t legacy_angle_delta(uint32_t current, uint32_t target)
{
    uint32_t delta = (target - current) & 0xffffu;
    if (delta > 0x8000u) delta = 0u - delta;
    return (int32_t)delta;
}

static void begin_tick(GameEnvironmentRecord *record, GameEnvironmentModel *model,
                       unsigned slot, uint16_t count)
{
    const uint32_t type = read_u32(record->input + TYPE_OFFSET);
    for (unsigned i = 0; i < 11; ++i) {
        const unsigned out = scalar_output_offsets[i];
        const float start = read_float(record->output + out);
        const float target = read_float(record->input + scalar_input_offsets[i]);
        /* Match the original double-precision subtract/divide/add, then float store. */
        const double next = (double)start +
            ((double)target - (double)start) / (double)count;
        model->interpolation[slot].scalar_start[i] = start;
        model->interpolation[slot].scalar_end[i] = (float)next;
    }
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t current = read_u32(record->output + 0x24u + i * 4u);
        const uint32_t target = read_u32(record->input + angle_input_offsets[i]);
        const int32_t delta = legacy_angle_delta(current, target);
        const int32_t step = delta / (int32_t)count;
        model->interpolation[slot].angle_start[i] = current;
        model->interpolation[slot].angle_end[i] = current + (uint32_t)step;
        model->interpolation[slot].angle_step[i] = step;
    }
    write_u32(record->output, type <= 3u ? 1u : 2u);
    if (type > 10u) {
        memcpy(record->output + 0x38u, record->input + 0x10u, 0x0cu);
        memcpy(record->output + 0x44u, record->input + 0x0cu, 4u);
    }
}

static void interpolate(GameEnvironmentRecord *record, GameEnvironmentModel *model,
                        unsigned slot, unsigned steps)
{
    uint16_t count = read_u16(record->input + COUNT_OFFSET);
    if (count == 0u) return;

    GameEnvironmentInterpolation *state = &model->interpolation[slot];
    if (!state->observed || state->observed_mode != 5u || state->observed_count != count ||
        memcmp(state->observed_input, record->input, sizeof state->observed_input) != 0) {
        state->phase = 0;
    }
    memcpy(state->observed_input, record->input, sizeof state->observed_input);
    state->observed = true;

    if (state->phase == 0u) begin_tick(record, model, slot, count);
    const unsigned phase = state->phase + 1u;
    for (unsigned i = 0; i < 11; ++i) {
        float value;
        if (phase == steps) value = state->scalar_end[i];
        else value = (float)((double)state->scalar_start[i] +
            ((double)state->scalar_end[i] - state->scalar_start[i]) * phase / steps);
        write_float(record->output + scalar_output_offsets[i], value);
    }
    for (unsigned i = 0; i < 2; ++i) {
        const int32_t fraction = (int32_t)(((int64_t)state->angle_step[i] * phase) /
                                           (int64_t)steps);
        const uint32_t value = phase == steps ? state->angle_end[i] :
            state->angle_start[i] + (uint32_t)fraction;
        write_u32(record->output + 0x24u + i * 4u, value);
    }

    state->phase = phase;
    if (phase == steps) {
        state->phase = 0;
        --count;
        write_u16(record->input + COUNT_OFFSET, count);
        if (count == 0u) write_u16(record->input + MODE_OFFSET, 3u);
    }
    state->observed_mode = read_u16(record->input + MODE_OFFSET);
    state->observed_count = read_u16(record->input + COUNT_OFFSET);
    memcpy(state->observed_input, record->input, sizeof state->observed_input);
}

void game_environment_model_step(GameEnvironmentModel *model, unsigned steps_per_tick)
{
    if (model == NULL || steps_per_tick == 0u) return;

    for (unsigned slot = 0; slot < GAME_ENVIRONMENT_RECORDS; ++slot) {
        GameEnvironmentRecord *record = &model->records[slot];
        const uint16_t mode = read_u16(record->input + MODE_OFFSET);
        switch (mode) {
        case 3: /* Idle: retain the previously published light. */
            write_u16(record->input + COUNT_OFFSET, 0u);
            model->interpolation[slot].phase = 0;
            break;
        case 4: /* Install a new light immediately. */
            setup(record);
            model->interpolation[slot].phase = 0;
            break;
        case 5: /* Move toward the requested light over count authored ticks. */
            interpolate(record, model, slot, steps_per_tick);
            break;
        case 6:
            write_u16(record->input + COUNT_OFFSET, 0u);
            write_u32(record->output, 0u);
            model->interpolation[slot].phase = 0;
            break;
        default:
            model->interpolation[slot].phase = 0;
            break;
        }
        if (mode == 4u || mode == 5u || mode == 6u) {
            memcpy(record->working, record->output, GAME_ENVIRONMENT_OUTPUT_SIZE);
        }
        model->interpolation[slot].observed_mode = read_u16(record->input + MODE_OFFSET);
        model->interpolation[slot].observed_count = read_u16(record->input + COUNT_OFFSET);
        memcpy(model->interpolation[slot].observed_input, record->input,
               sizeof model->interpolation[slot].observed_input);
        model->interpolation[slot].observed = true;
    }
}

/* Guest-memory adapter. The model above has no dependency on interception. */
static GameEnvironmentModel environment_model;
static const uint32_t input_base = 0x009d4c98u;
static const uint32_t output_base = 0x009d5240u;
static const uint32_t working_base = 0x009d50d8u;
static const uint32_t input_stride = 0x3cu;
static const uint32_t output_stride = 0x58u;

static void model_from_guest(void)
{
    for (unsigned i = 0; i < GAME_ENVIRONMENT_RECORDS; ++i) {
        GameEnvironmentRecord *r = &environment_model.records[i];
        memcpy(r->input, recomp_memory(input_base + i * input_stride, sizeof r->input), sizeof r->input);
        memcpy(r->output, recomp_memory(output_base + i * output_stride, sizeof r->output), sizeof r->output);
        memcpy(r->working, recomp_memory(working_base + i * output_stride, sizeof r->working), sizeof r->working);
    }
}

static void model_to_guest(void)
{
    for (unsigned i = 0; i < GAME_ENVIRONMENT_RECORDS; ++i) {
        GameEnvironmentRecord *r = &environment_model.records[i];
        memcpy(recomp_memory(input_base + i * input_stride, sizeof r->input), r->input, sizeof r->input);
        memcpy(recomp_memory(output_base + i * output_stride, sizeof r->output), r->output, sizeof r->output);
        memcpy(recomp_memory(working_base + i * output_stride, sizeof r->working), r->working, sizeof r->working);
    }
}

static void call_guest(uint32_t address)
{
    const uint32_t saved_esp = recomp_runtime.registers.esp;
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = 0;
    recomp_dispatch_indirect_site(address, saved_esp, __FILE__, __LINE__);
}

static void transition_entry(void)
{
    const unsigned steps = recomp_game_scheduler_hz() / 60u;
    model_from_guest();
    game_environment_model_step(&environment_model, steps);
    model_to_guest();
    recomp_runtime.registers.esp += 4u;
}

static void transition_gate(void)
{
    if (*recomp_memory(0x004d9688u, 1u) == 1u) {
        if (*recomp_memory(0x004d56efu, 1u) == 0u) call_guest(0x000c0500u);
        for (unsigned i = 0; i < GAME_ENVIRONMENT_RECORDS; ++i) {
            memcpy(recomp_memory(output_base + i * output_stride, output_stride),
                   recomp_memory(working_base + i * output_stride, output_stride), output_stride);
        }
    }
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = 0;
    call_guest(0x000c0900u);
    recomp_runtime.registers.ecx = *recomp_memory_u32(recomp_runtime.registers.esp);
    recomp_runtime.registers.esp += 8u;
}

static void environment_update(void)
{
    const uint32_t frame = *recomp_ebp_register();
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = saved_ebx;
    recomp_runtime.registers.ebx &= ~0xffu;
    if (*recomp_memory_u16(0x004d968eu) == 0x0eu &&
        *recomp_memory(0x004d9688u, 1u) == 1u) *recomp_memory(0x004d9680u, 1u) = 200u;

    const uint8_t selection = *recomp_memory(0x004d56e4u, 1u);
    const uint8_t selected = *recomp_memory(0x004d92f0u + selection, 1u);
    if (*recomp_memory_u32(0x009d4d88u) != selected ||
        *recomp_memory_u32(0x009d5508u) != selection) {
        *recomp_memory(0x009d550cu, 1u) = 0;
        *recomp_memory(0x009d4f34u, 1u) = 0;
        *recomp_memory(0x009d53a0u, 1u) = 0;
        *recomp_memory(0x00b3baacu, 1u) = 0;
    }
    call_guest(0x000c0490u);
    if (*recomp_memory(0x00b3baacu, 1u) != 0u &&
        *recomp_memory(0x004d9688u, 1u) != 1u) *recomp_memory(0x00b3baacu, 1u) = 0;
    *recomp_ebp_register() = frame;
    call_guest(0x000c1160u);
    const uint8_t final_selection = *recomp_memory(0x004d56e4u, 1u);
    *recomp_memory_u32(0x009d5508u) = final_selection;
    *recomp_memory_u32(0x009d4d88u) = *recomp_memory(0x004d92f0u + final_selection, 1u);
    *recomp_memory(0x009d5238u, 1u) = *recomp_memory(0x004d56dau, 1u);
    *recomp_memory(0x009d5239u, 1u) = *recomp_memory(0x004d9688u, 1u) == 1u;
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esp += 8u;
}

RecompFunction recomp_game_environment_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x000c03e0u: return environment_update;
    case 0x000c0490u: return transition_gate;
    case 0x000c0500u: return transition_entry;
    default: return NULL;
    }
}
