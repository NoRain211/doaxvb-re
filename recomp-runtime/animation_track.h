#ifndef DOAXBV_ANIMATION_TRACK_H
#define DOAXBV_ANIMATION_TRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct RecompAnimationCurve {
    float constant;
    float linear;
    float quadratic;
    float cubic;
} RecompAnimationCurve;

float recomp_animation_value(uint32_t packed);
float recomp_animation_tangent(uint32_t packed);
float recomp_animation_curvature(uint16_t packed);

/* Record includes the following endpoint when the form requires it. Duration
   belongs to the cursor; callers must not infer it from the next record. */
bool recomp_animation_curve(unsigned form, const uint8_t *record, size_t size,
    uint32_t duration, RecompAnimationCurve *curve);
float recomp_animation_curve_sample(const RecompAnimationCurve *curve, double x);

/* Immutable scalar sampling, with an independent scan from the track start.
   This is an integer curve sample, not the game's channel-aware pose blend.
   An absent track returns default_value; malformed/truncated tracks fail. */
bool recomp_animation_track_sample(const uint8_t *track, size_t size,
    uint32_t tick, float default_value, float *value);

#endif
