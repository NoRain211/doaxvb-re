#include "animation_track.h"

#include <string.h>

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
}

static float float_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

float recomp_animation_value(uint32_t packed)
{
    uint32_t sign = (packed & 0x80000u) << 12;
    uint32_t exponent = (packed >> 16) & 7u;
    uint32_t fraction = packed & 0xffffu;

    if (exponent != 0u) {
        return float_bits(sign | ((exponent + 126u) << 23) | (fraction << 7));
    }
    return (float)fraction * (sign ? -0x1p-16f : 0x1p-16f);
}

float recomp_animation_tangent(uint32_t packed)
{
    uint32_t sign = (packed & 0x800000u) << 8;
    uint32_t exponent = (packed >> 19) & 15u;
    uint32_t fraction = packed & 0x7ffffu;

    if (exponent != 0u) {
        return float_bits(sign | ((exponent + 112u) << 23) | (fraction << 4));
    }
    if (fraction == 0u) return 0.0f;
    return float_bits(sign | (((fraction >> 16) + 105u) << 23) |
        ((fraction & 0xffffu) << 7));
}

float recomp_animation_curvature(uint16_t packed)
{
    uint32_t sign = (uint32_t)(packed & 0x8000u) << 16;
    uint32_t exponent = (packed >> 12) & 7u;
    uint32_t fraction = packed & 0xfffu;

    if (exponent != 0u) {
        return float_bits(sign | ((exponent + 113u) << 23) | (fraction << 11));
    }
    if (fraction == 0u) return 0.0f;
    return float_bits(sign | (((fraction >> 10) + 110u) << 23) |
        ((fraction & 0x3ffu) << 13));
}

static float record_value(const uint8_t *record)
{
    uint32_t packed = (uint32_t)(read_u16(record) & 0xf000u) << 4;
    return recomp_animation_value(packed | read_u16(record + 2));
}

bool recomp_animation_curve(unsigned form, const uint8_t *record, size_t size,
    uint32_t duration, RecompAnimationCurve *curve)
{
    static const size_t required[] = {14u, 8u, 10u, 4u};
    RecompAnimationCurve result = {0};

    if (form >= 4u || record == NULL || curve == NULL ||
        size < required[form] || (form != 3u && duration == 0u)) return false;

    result.constant = record_value(record);
    if (form == 0u) {
        uint16_t high = read_u16(record + 8);
        float start = recomp_animation_tangent(
            read_u16(record + 4) | (uint32_t)(high & 0xffu) << 16);
        float end = recomp_animation_tangent(
            read_u16(record + 6) | (uint32_t)(high >> 8) << 16);
        float length = (float)duration;
        float inverse = (float)(1.0 / (double)length);
        double slope = ((double)record_value(record + 10) - result.constant) * inverse;

        /* These float stores and double intermediates match the generated
           x87 model. Rearranging the Hermite formula changes integer poses. */
        result.linear = start;
        result.cubic = (float)(((double)end + start + slope * -2.0) /
            ((double)length * length));
        result.quadratic = (float)((slope * 3.0 -
            ((double)start + start + end)) * inverse);
    } else if (form == 1u) {
        result.linear = (float)(((double)record_value(record + 4) -
            result.constant) / (double)duration);
    } else if (form == 2u) {
        double bend = recomp_animation_curvature(read_u16(record + 4));
        result.quadratic = (float)-bend;
        result.linear = (float)((double)duration * bend +
            ((double)record_value(record + 6) - result.constant) / (double)duration);
    }
    *curve = result;
    return true;
}

float recomp_animation_curve_sample(const RecompAnimationCurve *curve, double x)
{
    return (float)((((double)curve->cubic * x + curve->quadratic) * x +
        curve->linear) * x + curve->constant);
}

bool recomp_animation_track_sample(const uint8_t *track, size_t size,
    uint32_t tick, float default_value, float *value)
{
    static const size_t stride[] = {10u, 4u, 6u, 0u};
    uint32_t start = 0u;
    size_t offset = 0u;

    if (value == NULL || tick > UINT16_MAX) return false;
    if (track == NULL) {
        if (size != 0u) return false;
        *value = default_value;
        return true;
    }
    while (offset <= size && size - offset >= 4u) {
        const uint8_t *record = track + offset;
        uint16_t header = read_u16(record);
        unsigned form = header & 3u;
        uint32_t duration = (header >> 2) & 0x3ffu;
        uint32_t end = start + duration;

        if ((form == 0u && duration == 0u) || form == 3u) {
            if (form == 3u) {
                if (duration == 0u ||
                    start + ((tick - start) / duration + 1u) * duration > UINT16_MAX)
                    return false;
            }
            *value = record_value(record);
            return true;
        }
        /* Guest cursors wrap at 16 bits. A visual reader cannot silently
           interpret that as a valid monotonic clip; the caller owns loops. */
        if (end > UINT16_MAX) return false;
        if (tick < end) {
            RecompAnimationCurve curve;
            if (!recomp_animation_curve(form, record, size - offset, duration, &curve))
                return false;
            *value = recomp_animation_curve_sample(&curve, (double)tick - start);
            return true;
        }
        if (stride[form] > size - offset) return false;
        offset += stride[form];
        start = end;
    }
    return false;
}
