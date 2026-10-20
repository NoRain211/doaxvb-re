#include "game_environment.h"

#include <stdio.h>
#include <stdlib.h>
#define assert(test) do { if (!(test)) { fprintf(stderr, "environment check failed at line %d\n", __LINE__); abort(); } } while (0)
#include <string.h>

static const unsigned scalar_outputs[11] = {
    0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x2c, 0x30, 0x34,
};
static const unsigned scalar_inputs[11] = {
    0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x00, 0x04, 0x08,
};

static void put16(uint8_t *p, uint16_t value) { memcpy(p, &value, sizeof value); }
static uint16_t get16(const uint8_t *p) { uint16_t v; memcpy(&v, p, sizeof v); return v; }
static void put32(uint8_t *p, uint32_t value) { memcpy(p, &value, sizeof value); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static void putf(uint8_t *p, float value) { memcpy(p, &value, sizeof value); }
static float getf(const uint8_t *p) { float v; memcpy(&v, p, sizeof v); return v; }

static void test_setup_offsets_and_reserved(void)
{
    GameEnvironmentModel m = {0};
    GameEnvironmentRecord *r = &m.records[0];
    for (unsigned i = 0; i < sizeof r->output; ++i) r->output[i] = 0xa5;
    for (unsigned i = 0; i < 13; ++i) put32(r->input + i * 4, 0x3f000000u + i);
    put32(r->input + 0x34, 11u);
    put16(r->input + 0x38, 4u);
    game_environment_model_step(&m, 1);
    assert(get16(r->input + 0x38) == 3);
    assert(get16(r->input + 0x3a) == 0);
    assert(get32(r->output) == 2);
    for (unsigned i = 0; i < 11; ++i)
        assert(get32(r->output + scalar_outputs[i]) == get32(r->input + scalar_inputs[i]));
    assert(get32(r->output + 0x24) == get32(r->input + 0x2c));
    assert(get32(r->output + 0x28) == get32(r->input + 0x30));
    assert(memcmp(r->output + 0x38, r->input + 0x10, 0x0c) == 0);
    assert(get32(r->output + 0x44) == get32(r->input + 0x0c));
    assert(r->output[0x48] == 0xa5 && r->output[0x57] == 0xa5);
    assert(memcmp(r->working, r->output, sizeof r->output) == 0);
}

static uint32_t first_tick_value(unsigned steps)
{
    GameEnvironmentModel m = {0};
    GameEnvironmentRecord *r = &m.records[0];
    put16(r->input + 0x38, 5);
    put16(r->input + 0x3a, 2);
    putf(r->input + 0x0c, 5.0f);
    putf(r->output + 0x04, 1.0f);
    put32(r->input + 0x2c, 0x0000ffffu);
    put32(r->output + 0x24, 0x0000fff0u);
    for (unsigned i = 0; i < steps; ++i) game_environment_model_step(&m, steps);
    assert(get16(r->input + 0x3a) == 1);
    assert(get32(r->output + 0x24) == 0x0000fff7u);
    return get32(r->output + 0x04);
}

static void test_interpolation_rates_and_angles(void)
{
    const uint32_t one = first_tick_value(1);
    assert(one == first_tick_value(2));
    assert(one == first_tick_value(4));
    float expected = 3.0f;
    assert(one == get32((const uint8_t *)&expected));

    GameEnvironmentModel m = {0};
    GameEnvironmentRecord *r = &m.records[0];
    put16(r->input + 0x38, 5);
    put16(r->input + 0x3a, 1);
    put32(r->input + 0x2c, 0x0000ffffu);
    put32(r->output + 0x24, 0x0000fff0u);
    game_environment_model_step(&m, 1);
    assert(get32(r->output + 0x24) == 0x0000ffffu);

    memset(&m, 0, sizeof m);
    r = &m.records[0];
    put16(r->input + 0x38, 5);
    put16(r->input + 0x3a, 2);
    put32(r->input + 0x2c, 0x0000ffecu);
    put32(r->output + 0x24, 100u);
    game_environment_model_step(&m, 1);
    assert(get32(r->output + 0x24) == 0xffff80a0u);
}

static void test_count_crossing_and_external_restart(void)
{
    GameEnvironmentModel m = {0};
    GameEnvironmentRecord *r = &m.records[0];
    put16(r->input + 0x38, 5);
    put16(r->input + 0x3a, 2);
    putf(r->input + 0x0c, 8.0f);
    putf(r->output + 0x04, 0.0f);
    game_environment_model_step(&m, 4);
    assert(getf(r->output + 0x04) == 1.0f);
    putf(r->input + 0x0c, 12.0f); /* External target edit restarts from the current output. */
    for (unsigned i = 0; i < 4; ++i) game_environment_model_step(&m, 4);
    assert(getf(r->output + 0x04) == 6.5f);
    assert(get16(r->input + 0x3a) == 1);

    put16(r->input + 0x3a, 2); /* External count edit also restarts the fraction. */
    putf(r->input + 0x0c, 10.0f);
    game_environment_model_step(&m, 4);
    for (unsigned i = 1; i < 4; ++i) game_environment_model_step(&m, 4);
    assert(getf(r->output + 0x04) == 8.25f);
    assert(get16(r->input + 0x3a) == 1 && get16(r->input + 0x38) == 5);

    put16(r->input + 0x38, 4); /* External mode change takes the setup path. */
    putf(r->input + 0x0c, 14.0f);
    game_environment_model_step(&m, 4);
    assert(getf(r->output + 0x04) == 14.0f);
    assert(get16(r->input + 0x38) == 3);
}

static void test_modes_and_full_copy_rules(void)
{
    GameEnvironmentModel m = {0};
    GameEnvironmentRecord *r = &m.records[0];
    memset(r->output, 0x5a, sizeof r->output);
    memset(r->working, 0xc3, sizeof r->working);
    put16(r->input + 0x38, 6);
    put16(r->input + 0x3a, 9);
    game_environment_model_step(&m, 2);
    assert(get16(r->input + 0x3a) == 0 && get32(r->output) == 0);
    assert(memcmp(r->working, r->output, sizeof r->output) == 0);

    memset(r->working, 0xc3, sizeof r->working);
    put16(r->input + 0x38, 3);
    put16(r->input + 0x3a, 9);
    game_environment_model_step(&m, 2);
    assert(get16(r->input + 0x3a) == 0);
    for (unsigned i = 0; i < sizeof r->working; ++i) assert(r->working[i] == 0xc3);

    put16(r->input + 0x38, 5);
    put16(r->input + 0x3a, 1);
    putf(r->input + 0x0c, 4.0f);
    putf(r->output + 0x04, 0.0f);
    game_environment_model_step(&m, 2);
    assert(memcmp(r->working, r->output, sizeof r->output) == 0);
}

int recomp_game_environment_test(void)
{
    test_setup_offsets_and_reserved();
    test_interpolation_rates_and_angles();
    test_count_crossing_and_external_restart();
    test_modes_and_full_copy_rules();
    return 1;
}
