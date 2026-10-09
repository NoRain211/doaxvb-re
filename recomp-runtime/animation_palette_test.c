#include "animation_palette.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Palette line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
    const RecompBoneMatrix bones[] = {
        {{0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1}},
        {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 4, 5, 1}}
    };
    const float offsets[][4] = {{1, 2, 3, 0}, {4, 5, 6, 0}};
    const uint8_t recipe[] = {2, 0, 128, 255, 1, 65, 255};
    RecompBoneMatrix output[4], scratch, unchanged[4];
    memset(output, 0xa5, sizeof output);
    CHECK(recomp_animation_build_palette(recipe, sizeof recipe, 2, bones, 2u, offsets, 2u,
        &bones[0], output, &scratch));
    CHECK(output[0].m[12] == 8 && output[0].m[13] == 21 && output[0].m[14] == 33);
    CHECK(output[1].m[12] == -1 && output[1].m[13] == -1 && output[1].m[14] == -1);
    CHECK(memcmp(&scratch, &output[1], sizeof scratch) == 0);
    memcpy(unchanged, output, sizeof output);
    CHECK(!recomp_animation_build_palette(recipe, sizeof recipe - 1u, 2, bones, 2u,
        offsets, 2u, &bones[0], output, &scratch));
    CHECK(memcmp(output, unchanged, sizeof output) == 0);
    CHECK(memcmp(&scratch, &output[1], sizeof scratch) == 0);
    CHECK(!recomp_animation_build_palette(recipe, sizeof recipe, 2, bones, 1u,
        offsets, 2u, &bones[0], output, &scratch));
    CHECK(!recomp_animation_build_palette(recipe, sizeof recipe, 2, bones, 2u,
        offsets, 1u, &bones[0], output, &scratch));
    const uint8_t continued[] = {1, 128, 255};
    CHECK(recomp_animation_build_palette(continued, sizeof continued, 1, bones, 2u,
        offsets, 2u, &bones[0], output, &scratch));
    CHECK(output[0].m[12] == 8 && output[0].m[13] == 21);
    const uint8_t invalid_count[] = {5};
    CHECK(!recomp_animation_build_palette(invalid_count, sizeof invalid_count, 1, bones, 2u,
        offsets, 2u, &bones[0], output, &scratch));
    /* A declared but unused suffix must not be read or invented. */
    const uint8_t prefix[] = {2, 1, 255, 128};
    CHECK(recomp_animation_build_palette(prefix, sizeof prefix, 1, bones, 2u,
        offsets, 2u, &bones[0], output, &scratch));
    CHECK(memcmp(output, bones+1, sizeof *output) == 0);
    CHECK(!recomp_animation_build_palette(prefix, sizeof prefix, 2, bones, 2u,
        offsets, 2u, &bones[0], output, &scratch));
    puts("Animation palette tests passed");
    return 0;
}
