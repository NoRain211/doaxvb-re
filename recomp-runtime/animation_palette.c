#include "animation_palette.h"

#include <string.h>

bool recomp_animation_build_palette(const uint8_t *recipe, size_t recipe_size,
    unsigned output_count,
    const RecompBoneMatrix *bones, size_t bone_count,
    const float (*offsets)[4], size_t offset_count,
    const RecompBoneMatrix *initial, RecompBoneMatrix output[4],
    RecompBoneMatrix *final_scratch)
{
    RecompBoneMatrix palette[4], current;
    size_t next = 1u;
    if (recipe == NULL || recipe_size == 0u || recipe[0] > 4u ||
        output_count > recipe[0] || bones == NULL ||
        offsets == NULL || initial == NULL || output == NULL || final_scratch == NULL)
        return false;
    current = *initial;
    for (unsigned entry = 0u; entry < output_count; ++entry) {
        bool started = false;
        for (;;) {
            if (next == recipe_size) return false;
            unsigned operation = recipe[next++];
            if (operation == 255u) {
                if (!started) return false;
                break;
            }
            started = true;
            unsigned index = operation & 63u;
            if ((operation & 192u) == 0u) {
                if (index >= bone_count) return false;
                current = bones[index];
            } else {
                if (index >= offset_count) return false;
                float sign = (operation & 128u) != 0u ? 1.0f : -1.0f;
                float x = sign * offsets[index][0];
                float y = sign * offsets[index][1];
                float z = sign * offsets[index][2];
                for (unsigned column = 0u; column < 4u; ++column) {
                    current.m[12u + column] = (float)((double)x * current.m[column] +
                        (double)y * current.m[4u + column] +
                        (double)z * current.m[8u + column] + current.m[12u + column]);
                }
            }
        }
        palette[entry] = current;
    }
    memcpy(output, palette, output_count * sizeof *output);
    *final_scratch = current;
    return true;
}
