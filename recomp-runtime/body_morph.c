#include "body_morph.h"

#include <string.h>

RecompMorphKind recomp_morph_kind(uint32_t shape_a, uint32_t shape_b, float third_weight)
{
    if (!(third_weight == 0.0f)) return RECOMP_MORPH_THREE;
    return shape_a == shape_b ? RECOMP_MORPH_ONE : RECOMP_MORPH_TWO;
}

void recomp_morph_blend(uint8_t *buffer, size_t size, uint32_t count, const uint32_t *offsets,
    RecompMorphKind kind, bool mirrored,
    const float *a, float wa, const float *b, float wb, const float *c, float wc)
{
    for (uint32_t n = 0; n < count; ++n) {
        const uint32_t at = offsets[n];
        if ((size_t)at + 12u > size) continue;
        float out[3];
        for (unsigned axis = 0; axis < 3; ++axis) {
            const size_t i = (size_t)n * 3u + axis;
            double value;
            switch (kind) {
            case RECOMP_MORPH_ONE:
                value = a[i];
                break;
            case RECOMP_MORPH_TWO:
                value = (double)wa * a[i] + (double)wb * b[i];
                break;
            default:
                value = ((double)wa * a[i] + (double)wb * b[i]) + (double)wc * c[i];
                break;
            }
            out[axis] = (float)(axis == 0u && mirrored ? -value : value);
        }
        /* The unmirrored copy moves the stored bits as they are. */
        if (kind == RECOMP_MORPH_ONE && !mirrored) memcpy(out, a + (size_t)n * 3u, sizeof out);
        memcpy(buffer + at, out, sizeof out);
    }
}
