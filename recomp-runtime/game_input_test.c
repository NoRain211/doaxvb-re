#include "game_input.h"
#include "game_scheduler.h"

#include <stdio.h>
#include <string.h>

static int check_guest_delivery(void)
{
    static uint8_t tables[0xc0], pads[0x300];
    const RecompMemoryRegion regions[] = {
        {0x002c02a0u, sizeof tables, tables},
        {0x005f2e80u, sizeof pads, pads},
    };
    memset(tables, 0, sizeof tables);
    memset(pads, 0, sizeof pads);
    recomp_runtime_init(regions, 2, NULL, 0, NULL, 0);
    recomp_game_scheduler_reset(120);
    *recomp_memory_u32(0x002c02a0u) = 0x10;
    *recomp_memory_u32(0x002c02a4u) = 0x10;
    *recomp_memory_u32(0x002c02ecu) = 0x20;
    *recomp_memory_u32(0x002c02f0u) = 1;
    *recomp_memory_u32(0x002c02f4u) = 0x100;
    *recomp_memory_u32(0x002c0320u) = 1;
    *recomp_memory_u32(0x002c0324u) = 0x10000;
    *recomp_memory_u32(0x005f3080u) = 1;
    *recomp_memory_u16(0x005f2e9du) = 0x10;
    *recomp_memory(0x005f2e9fu, 1) = 31;
    *recomp_memory(0x005f2ea0u, 1) = 128;
    const int16_t axes[] = {-8192, 8191, 8192, -8191};
    recomp_guest_store(0x005f2ea7u, axes, sizeof axes);
    recomp_runtime.registers.esp = 0x1234;
    recomp_game_input_update();
    RecompGameInput state, aggregate;
    recomp_guest_load(&state, 0x005f3088u, sizeof state);
    recomp_guest_load(&aggregate, 0x005f3138u, sizeof aggregate);
    int passed = state.held == 0x10110 && state.pressed == 0x10110 &&
        state.repeated == 0x110 && state.repeat_delay == 40 &&
        state.pressure[0] == 0 && state.pressure[1] == 128 &&
        state.axes[0] == 8192 && state.axes[1] == 0 &&
        state.axes[2] == -8192 && state.axes[3] == 0 &&
        aggregate.held == state.held && aggregate.pressed == state.pressed &&
        aggregate.pressure[1] == 0 && recomp_runtime.registers.esp == 0x1238;
    *recomp_memory_u32(0x005f3080u) = 0;
    recomp_game_input_update();
    recomp_guest_load(&state, 0x005f3088u, sizeof state);
    passed &= state.held == 0 && state.pressed == 0 && state.released == 0x10110 &&
        state.repeat_delay == 0 && state.pressure[1] == 0 && state.axes[0] == 0;
    recomp_runtime_init(NULL, 0, NULL, 0, NULL, 0);
    recomp_game_scheduler_reset(60);
    if (!passed) fprintf(stderr, "game input: mapping, deadzone or disconnect delivery differs\n");
    return passed;
}

int recomp_game_input_test(void)
{
    for (unsigned hz = 60; hz <= 120; hz += 60) {
        RecompGameInput state = {0};
        const unsigned scale = hz / 60;
        for (unsigned tick = 0; tick <= 30 * scale; ++tick) {
            recomp_game_input_buttons(&state, 0x10, hz);
            const bool repeat = tick == 0 ||
                (tick >= 20 * scale && (tick - 20 * scale) % (5 * scale) == 0);
            if (state.pressed != (tick == 0 ? 0x10u : 0) ||
                state.repeated != (repeat ? 0x10u : 0) || state.released != 0) {
                fprintf(stderr, "game input: edge/repeat cadence at %u Hz tick %u\n", hz, tick);
                return 0;
            }
        }
        recomp_game_input_buttons(&state, 0, hz);
        if (state.released != 0x10 || state.pressed != 0 ||
            state.repeated != 0 || state.repeat_delay != 0) return 0;
        recomp_game_input_buttons(&state, 0x10001, hz);
        if (state.pressed != 0x10001 || state.repeated != 1) return 0;
        recomp_game_input_buttons(&state, 1, hz);
        if (state.released != 0x10000 || state.repeat_delay != 0) return 0;
    }
    return check_guest_delivery();
}
