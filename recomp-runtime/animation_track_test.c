#include "animation_track.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Animation track: line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void word(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

/* Synthetic packed values, not game fixtures. */
static void record(uint8_t *bytes, unsigned form, unsigned duration, uint32_t value)
{
    word(bytes, (uint16_t)(form | duration << 2 | (value >> 16) << 12));
    word(bytes + 2, (uint16_t)value);
}

int main(void)
{
    uint8_t track[32] = {0};
    uint8_t before[sizeof track];
    RecompAnimationCurve curve;
    float value;

    /* Every value encoding, including signed zero and exponent transitions. */
    for (uint32_t code = 0; code < 0x100000u; ++code) {
        unsigned exponent = (code >> 16) & 7u;
        double fraction = (double)(code & 0xffffu) / 65536.0;
        float expected = (float)(exponent ? ldexp(1.0 + fraction, (int)exponent - 1) : fraction);
        float actual = recomp_animation_value(code);
        if (code & 0x80000u) expected = -expected;
        CHECK(memcmp(&actual, &expected, sizeof actual) == 0);
    }
    /* The small-exponent escape forms are custom, not IEEE subnormals. */
    for (uint32_t code = 0; code < 0x1000000u; ++code) {
        unsigned exponent = (code >> 19) & 15u;
        uint32_t fraction = code & 0x7ffffu;
        float expected = exponent
            ? (float)ldexp(1.0 + (double)fraction / 524288.0, (int)exponent - 15)
            : fraction ? (float)ldexp(1.0 + (double)(fraction & 0xffffu) / 65536.0,
                (int)(fraction >> 16) - 22) : 0.0f;
        float actual = recomp_animation_tangent(code);
        if ((code & 0x800000u) && (exponent || fraction)) expected = -expected;
        CHECK(memcmp(&actual, &expected, sizeof actual) == 0);
    }
    for (uint32_t code = 0; code < 0x10000u; ++code) {
        unsigned exponent = (code >> 12) & 7u;
        uint32_t fraction = code & 0xfffu;
        float expected = exponent
            ? (float)ldexp(1.0 + (double)fraction / 4096.0, (int)exponent - 14)
            : fraction ? (float)ldexp(1.0 + (double)(fraction & 0x3ffu) / 1024.0,
                (int)(fraction >> 10) - 17) : 0.0f;
        float actual = recomp_animation_curvature((uint16_t)code);
        if ((code & 0x8000u) && (exponent || fraction)) expected = -expected;
        CHECK(memcmp(&actual, &expected, sizeof actual) == 0);
    }
    CHECK(recomp_animation_tangent(0x800000u) == 0.0f);
    CHECK(!signbit(recomp_animation_tangent(0x800000u)));
    CHECK(recomp_animation_tangent(1u) == (float)ldexp(1.0 + 1.0 / 65536.0, -22));
    CHECK(recomp_animation_tangent(0x80000u) == 0x1p-14f);
    CHECK(recomp_animation_tangent(0x880000u) == -0x1p-14f);
    CHECK(recomp_animation_curvature(0x8000u) == 0.0f);
    CHECK(!signbit(recomp_animation_curvature(0x8000u)));
    CHECK(recomp_animation_curvature(1u) == (float)ldexp(1.0 + 1.0 / 1024.0, -17));
    CHECK(recomp_animation_curvature(0x1000u) == 0x1p-13f);
    CHECK(recomp_animation_curvature(0x9000u) == -0x1p-13f);

    /* Zero-tangent Hermite from 1 to 3 over four ticks. */
    record(track, 0u, 4u, 0x10000u);
    record(track + 10, 0u, 0u, 0x28000u);
    memcpy(before, track, sizeof track);
    CHECK(recomp_animation_curve(0u, track, sizeof track, 4u, &curve));
    CHECK(curve.constant == 1.0f && curve.linear == 0.0f);
    CHECK(curve.quadratic == 0.375f && curve.cubic == -0.0625f);
    CHECK(recomp_animation_track_sample(track, 14u, 0u, 0.0f, &value) && value == 1.0f);
    CHECK(recomp_animation_track_sample(track, 14u, 2u, 0.0f, &value) && value == 2.0f);
    CHECK(recomp_animation_track_sample(track, 14u, 4u, 0.0f, &value) && value == 3.0f);
    CHECK(recomp_animation_track_sample(track, 14u, 100u, 0.0f, &value) && value == 3.0f);
    /* Backward sampling uses no mutable cursor cache. */
    CHECK(recomp_animation_track_sample(track, 14u, 1u, 0.0f, &value) && value == 1.3125f);
    CHECK(memcmp(track, before, sizeof track) == 0);
    CHECK(!recomp_animation_track_sample(track, 13u, 2u, 0.0f, &value));

    memset(track, 0, sizeof track);
    record(track, 1u, 2u, 0x10000u);
    record(track + 4, 1u, 2u, 0x28000u);
    record(track + 8, 0u, 0u, 0x30000u);
    CHECK(recomp_animation_track_sample(track, 12u, 1u, 0.0f, &value) && value == 2.0f);
    CHECK(recomp_animation_track_sample(track, 12u, 2u, 0.0f, &value) && value == 3.0f);
    CHECK(recomp_animation_track_sample(track, 12u, 3u, 0.0f, &value) && value == 3.5f);
    CHECK(recomp_animation_track_sample(track, 12u, 4u, 0.0f, &value) && value == 4.0f);

    memset(track, 0, sizeof track);
    record(track, 2u, 4u, 0x10000u);
    word(track + 4, 0x7000u); /* curvature 1/128 */
    record(track + 6, 0u, 0u, 0x28000u);
    CHECK(recomp_animation_curve(2u, track, 10u, 4u, &curve));
    CHECK(curve.quadratic == -0x1p-7f && curve.cubic == 0.0f);
    CHECK(recomp_animation_curve_sample(&curve, 0.0) == 1.0f);
    CHECK(recomp_animation_curve_sample(&curve, 2.0) == 2.03125f);
    CHECK(recomp_animation_curve_sample(&curve, 4.0) == 3.0f);
    CHECK(!recomp_animation_curve(2u, track, 9u, 4u, &curve));
    CHECK(!recomp_animation_curve(2u, track, 10u, 0u, &curve));

    for (unsigned form = 1; form <= 2; ++form) {
        memset(track, 0, sizeof track);
        record(track, form, 0u, 0x10000u);
        record(track + (form == 1 ? 4 : 6), 0u, 0u, 0x28000u);
        value = 17;
        CHECK(!recomp_animation_track_sample(track, sizeof track, 0u, 0, &value));
        CHECK(value == 17);
    }
    record(track, 3u, 1u, 0x98000u);
    CHECK(recomp_animation_track_sample(track, 4u, 65534u, 0.0f, &value) && value == -1.5f);
    CHECK(!recomp_animation_track_sample(track, 4u, 65535u, 0.0f, &value));
    record(track, 3u, 0u, 0x10000u);
    CHECK(!recomp_animation_track_sample(track, 4u, 0u, 0.0f, &value));
    CHECK(recomp_animation_track_sample(NULL, 0u, 23u, 5.0f, &value) && value == 5.0f);
    CHECK(!recomp_animation_track_sample(NULL, 1u, 0u, 0.0f, &value));
    CHECK(!recomp_animation_track_sample(track, 4u, 65536u, 0.0f, &value));
    CHECK(!recomp_animation_curve(4u, track, sizeof track, 1u, &curve));
    puts("Animation track tests passed");
    return 0;
}
