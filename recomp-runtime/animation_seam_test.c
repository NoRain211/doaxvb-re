#include "animation_seam.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"Seam line %d: %s\n",__LINE__,#c); return 1; } } while (0)
int main(void)
{
    RecompBoneMatrix source = {{1,0,0,0, 0,1,0,0, 0,0,1,0, 5,7,9,1}};
    RecompBoneMatrix destination = source, relative, matrix;
    destination.m[12] = 2; destination.m[13] = 3; destination.m[14] = 4;
    CHECK(recomp_animation_seam_relative(&source, &destination, &relative));
    CHECK(relative.m[12] == 3 && relative.m[13] == 4 && relative.m[14] == 5);
    float reference[10][4] = {{1,0,0,0},{1,0,0,0},{0,1,0,0},{0,1,0,0},
        {0,0,1,0},{0,0,1,0},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1}};
    RecompSeamBasis basis;
    CHECK(recomp_animation_seam_basis(reference, &relative, &basis));
    const float coefficients[4] = {0,0,1,0};
    CHECK(recomp_animation_seam_matrix(&basis, coefficients, &matrix));
    float point[3] = {1,2,3}, normal[3] = {0,1,0}, vertex[6];
    recomp_animation_seam_vertex(&matrix, point, normal, vertex);
    CHECK(fabsf(vertex[0]-2.5f)<1e-6f && fabsf(vertex[1]-4)<1e-6f && fabsf(vertex[2]-5.5f)<1e-6f);
    CHECK(vertex[3] == 0 && fabsf(vertex[4]-1)<1e-6f && vertex[5] == 0);
    RecompBoneMatrix turned = {{0,1,0,0, -1,0,0,0, 0,0,1,0, 2,3,4,1}};
    CHECK(recomp_animation_seam_relative(&source, &turned, &relative));
    CHECK(relative.m[12] == 4 && relative.m[13] == -3 && relative.m[14] == 5);
    RecompBoneMatrix saved = relative;
    destination.m[0] = 0;
    CHECK(!recomp_animation_seam_relative(&source, &destination, &relative));
    CHECK(memcmp(&saved, &relative, sizeof saved) == 0);
    reference[0][0] = NAN;
    RecompSeamBasis saved_basis = basis;
    CHECK(!recomp_animation_seam_basis(reference, &relative, &basis));
    CHECK(memcmp(&saved_basis, &basis, sizeof basis) == 0);
    puts("Seam relative frame, control blend, normal and atomic failure tests passed");
    return 0;
}
