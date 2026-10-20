/* body_morph.c on made-up shapes: which blend a record asks for, the sums in
   the game's order and precision, the mirrored kind and bit-exact copies.
   Checked against the game itself with RECOMP_BODY_MORPH_VERIFY=1. */
#include "body_morph.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main(void)
{
    CHECK(recomp_morph_kind(1u, 1u, 0.0f) == RECOMP_MORPH_ONE);
    CHECK(recomp_morph_kind(1u, 1u, -0.0f) == RECOMP_MORPH_ONE);
    CHECK(recomp_morph_kind(1u, 2u, 0.0f) == RECOMP_MORPH_TWO);
    CHECK(recomp_morph_kind(1u, 1u, 0.5f) == RECOMP_MORPH_THREE);
    CHECK(recomp_morph_kind(1u, 2u, NAN) == RECOMP_MORPH_THREE);

    const float a[6] = {0.1f, 0.2f, 0.3f, -0.0f, 1e-30f, 3.0f};
    const float b[6] = {0.7f, 0.11f, -0.3f, 2.0f, 4.0f, -5.0f};
    const float c[6] = {0.01f, 0.02f, 0.03f, 0.04f, 0.05f, 0.06f};
    const uint32_t offsets[2] = {32u, 0u};
    uint8_t buffer[64];
    float out[3];

    /* One shape: the stored bits, at each vector's offset. */
    memset(buffer, 0, sizeof buffer);
    recomp_morph_blend(buffer, sizeof buffer, 2, offsets, RECOMP_MORPH_ONE, false, a, 0, NULL, 0, NULL, 0);
    CHECK(memcmp(buffer + 32, a, 12) == 0 && memcmp(buffer, a + 3, 12) == 0);

    /* Two: wa * a + wb * b in double, rounded once; mirrored negates x. */
    recomp_morph_blend(buffer, sizeof buffer, 2, offsets, RECOMP_MORPH_TWO, true, a, 0.503f, b, 0.497f, NULL, 0);
    memcpy(out, buffer + 32, 12);
    CHECK(out[0] == (float)-((double)0.503f * a[0] + (double)0.497f * b[0]));
    CHECK(out[1] == (float)((double)0.503f * a[1] + (double)0.497f * b[1]));

    /* Three: (wa * a + wb * b) + wc * c. */
    recomp_morph_blend(buffer, sizeof buffer, 2, offsets, RECOMP_MORPH_THREE, false, a, 0.25f, b, 0.5f, c, 0.25f);
    memcpy(out, buffer, 12);
    CHECK(out[2] == (float)(((double)0.25f * a[5] + (double)0.5f * b[5]) + (double)0.25f * c[5]));

    /* A vector past the buffer is skipped. */
    const uint32_t far[1] = {60u};
    memset(buffer, 0x5A, sizeof buffer);
    recomp_morph_blend(buffer, sizeof buffer, 1, far, RECOMP_MORPH_ONE, false, a, 0, NULL, 0, NULL, 0);
    CHECK(buffer[60] == 0x5A);

    if (!failures) printf("body morph: ok\n");
    return failures ? 1 : 0;
}
