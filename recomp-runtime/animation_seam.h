#ifndef DOAXBV_ANIMATION_SEAM_H
#define DOAXBV_ANIMATION_SEAM_H

#include "animation_palette.h"

/* Three direction pairs and four origin controls, supplied with the rig. */
typedef struct RecompSeamBasis {
    float rows[4][3][4];
} RecompSeamBasis;

bool recomp_animation_seam_relative(const RecompBoneMatrix *source,
    const RecompBoneMatrix *destination, RecompBoneMatrix *output);
bool recomp_animation_seam_basis(const float reference[10][4],
    const RecompBoneMatrix *relative, RecompSeamBasis *output);
bool recomp_animation_seam_matrix(const RecompSeamBasis *basis,
    const float coefficients[4], RecompBoneMatrix *output);
void recomp_animation_seam_vertex(const RecompBoneMatrix *matrix,
    const float position[3], const float normal[3], float output[6]);

#endif
