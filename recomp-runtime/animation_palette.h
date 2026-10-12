#ifndef DOAXBV_ANIMATION_PALETTE_H
#define DOAXBV_ANIMATION_PALETTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Row-vector affine matrices. Translation occupies elements 12..14. */
typedef struct RecompBoneMatrix { float m[16]; } RecompBoneMatrix;

/* A recipe loads a bone, applies signed local offsets, and terminates each
   output with 255. Inputs and initial scratch are immutable; failure leaves
   both outputs untouched. Only the mesh's required prefix is consumed; declared
   but unused entries may extend beyond a recipe row. No guest dependency. */
bool recomp_animation_build_palette(const uint8_t *recipe, size_t recipe_size,
    unsigned output_count,
    const RecompBoneMatrix *bones, size_t bone_count,
    const float (*offsets)[4], size_t offset_count,
    const RecompBoneMatrix *initial, RecompBoneMatrix output[4],
    RecompBoneMatrix *final_scratch);

#endif
