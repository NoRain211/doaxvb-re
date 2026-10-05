#ifndef DOAXBV_ANIMATION_ROTATION_H
#define DOAXBV_ANIMATION_ROTATION_H

/* Pure rotation math shared by the scalar-pose blend and skeleton assembler.
   Implemented beside the skeleton's matrix math; no guest/library callbacks. */
void recomp_animation_sine_cosine(float angle, float *sine, float *cosine);
void recomp_animation_blend_euler(const float a[3], const float b[3], float weight,
    float output[3]);

#endif
