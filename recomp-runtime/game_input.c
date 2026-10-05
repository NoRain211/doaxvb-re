#include "game_input.h"
#include "game_scheduler.h"

#include <string.h>

_Static_assert(sizeof(RecompGameInput) == 44, "guest input layout");

void recomp_game_input_buttons(RecompGameInput *state, uint32_t held, unsigned hz)
{
    const unsigned scale = hz == 120 ? 2 : 1;
    state->previous = state->held;
    state->held = held;
    state->pressed = held & ~state->previous;
    state->released = state->previous & ~held;
    state->repeated = 0;
    if (state->pressed != 0) {
        state->repeat_delay = 20 * scale;
        state->repeat_mask = state->pressed & 0xffffu;
        state->repeated = state->repeat_mask;
    } else if (state->repeat_delay != 0 && held == state->previous) {
        if (--state->repeat_delay == 0) {
            state->repeat_delay = 5 * scale;
            state->repeated = state->repeat_mask & held;
        }
    } else {
        state->repeat_delay = 0;
    }
}

static uint32_t map_buttons(uint32_t table, uint32_t buttons)
{
    uint32_t mapped = 0;
    for (; *recomp_memory_u32(table + 4u) != 0; table += 8u) {
        if ((*recomp_memory_u32(table) & buttons) != 0) {
            mapped |= *recomp_memory_u32(table + 4u);
        }
    }
    return mapped;
}

static uint32_t map_pressure(uint32_t table, uint8_t *pressure, unsigned threshold)
{
    uint32_t mapped = 0;
    for (; *recomp_memory_u32(table + 4u) != 0; table += 8u) {
        uint8_t *value = pressure + *recomp_memory_u32(table);
        if (threshold == 32 && *value < threshold) *value = 0;
        if (*value >= threshold) mapped |= *recomp_memory_u32(table + 4u);
    }
    return mapped;
}

/* Complete game-owned edge/repeat owner (0x000b5870). XAPI device polling
   stays with the existing input adapter. Scene delivery follows every poll. */
void recomp_game_input_update(void)
{
    RecompGameInput aggregate = {0};
    const uint32_t connected = *recomp_memory_u32(0x005f3080u);
    for (unsigned port = 0; port < 4; ++port) {
        const uint32_t address = 0x005f3088u + port * 44u;
        RecompGameInput state;
        recomp_guest_load(&state, address, sizeof state);
        uint32_t held = 0;
        if ((connected & (1u << port)) != 0) {
            const uint32_t raw = 0x005f2e99u + port * 128u;
            uint8_t *pressure = recomp_memory(raw + 6u, 8);
            held = map_buttons(0x002c02a0u, *recomp_memory_u16(raw + 4u));
            held |= map_pressure(0x002c02e8u, pressure, 32);
            held |= map_pressure(0x002c0320u, pressure, 128);
            memcpy(state.pressure, pressure, sizeof state.pressure);
            recomp_guest_load(state.axes, raw + 18u, 4);
            recomp_guest_load(state.axes + 2, raw + 14u, 4);
            for (unsigned axis = 0; axis < 4; ++axis) {
                if (state.axes[axis] > -8192 && state.axes[axis] < 8192) {
                    state.axes[axis] = 0;
                }
            }
        } else {
            memset(state.pressure, 0, sizeof state.pressure);
            memset(state.axes, 0, sizeof state.axes);
        }
        recomp_game_input_buttons(&state, held, recomp_game_scheduler_hz());
        recomp_guest_store(address, &state, sizeof state);
        aggregate.held |= state.held;
        aggregate.previous |= state.previous;
        aggregate.pressed |= state.pressed;
        aggregate.released |= state.released;
        aggregate.repeated |= state.repeated;
    }
    recomp_guest_store(0x005f3138u, &aggregate, sizeof aggregate);
    recomp_runtime.registers.esp += 4u;
}
