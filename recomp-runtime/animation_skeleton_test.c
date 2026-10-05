#include "animation_skeleton.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Skeleton line %d: %s\n", __LINE__, #c); return 1; } } while (0)

/* Synthetic rig: unit links, distinct end points and zero local offsets.
   No executable table, captured pose or extracted model is a test fixture. */
static void fixture(RecompSkeletonTables *t, float p[60], RecompSkeletonTargets *s)
{
    memset(t, 0, sizeof *t);
    memset(s, 0, sizeof *s);
    memset(p, 0, 60*sizeof(float));
    const unsigned slots[4][4] = {{3,5,4,6}, {7,8,9,255}, {12,11,10,13}, {18,14,22,255}};
    for (unsigned i = 0; i < 4; ++i) {
        RecompLimbTable *d = &t->limbs[i];
        d->end = (uint8_t)slots[i][0];
        d->parent = 2;
        d->proximal = (uint8_t)slots[i][1];
        d->middle = (uint8_t)slots[i][2];
        d->tip = (uint8_t)slots[i][3];
        d->position_channels[0] = 12;
        d->position_channels[1] = 13;
        d->position_channels[2] = 14;
        d->pole_channels[0] = 15;
        d->pole_channels[1] = 16;
        d->positive_bend = 1;
        d->upper_body = i & 1;
        s->lengths[i].proximal = s->lengths[i].distal = 1;
        s->lengths[i].alternate_proximal = s->lengths[i].alternate_distal = 1.1f;
    }
    p[14] = 1.5f;
    p[15] = -1.0f;
    s->terrain_disabled = 1;
}

int main(void)
{
    RecompSkeletonTables t, saved_tables;
    RecompSkeletonTargets s, saved_targets;
    RecompBoneMatrix a[32], b[32], before[32];
    float p[60], saved_channels[60];
    fixture(&t, p, &s);
    saved_tables = t; saved_targets = s; memcpy(saved_channels, p, sizeof p);
    CHECK(recomp_animation_solve_skeleton(&t, p, &s, a));
    CHECK(memcmp(&t, &saved_tables, sizeof t) == 0);
    CHECK(memcmp(&s, &saved_targets, sizeof s) == 0);
    CHECK(memcmp(p, saved_channels, sizeof p) == 0);
    CHECK(fabsf(a[3].m[14]-1.5f) < 0.001f);
    CHECK(fabsf(a[3].m[12]) < 0.001f);
    CHECK(recomp_animation_solve_skeleton(&t, p, &s, b));
    CHECK(memcmp(a, b, sizeof a) == 0);
    s.position[0] = 10;
    CHECK(recomp_animation_solve_skeleton(&t, p, &s, b));
    CHECK(fabsf(b[3].m[12]-a[3].m[12]-10) < 0.00001f);
    s = saved_targets;
    s.look_weight = 1;
    s.look_target[0] = 2; s.look_target[1] = 1; s.look_target[2] = 3;
    CHECK(recomp_animation_solve_skeleton(&t, p, &s, b));
    CHECK(fabsf(b[1].m[2]-a[1].m[2]) > 0.1f);
    s = saved_targets;
    s.upper_morph = 0.5f; s.lower_morph = 0.5f; s.derived_blend = 1;
    t.alternate_offsets[8][0] = 0.5f;
    CHECK(recomp_animation_solve_skeleton(&t, p, &s, b));
    CHECK(memcmp(a, b, sizeof a) != 0);
    CHECK(fabsf(b[8].m[12]) < 0.00001f); /* Morph anchor only controls the angle solve. */
    CHECK(fabsf(b[8].m[2]-a[8].m[2]) > 0.01f);
    t = saved_tables;
    s = saved_targets;
    s.terrain_disabled = 0; s.neck_height = 0.15f;
    t.offsets[19][2] = 0.3f;
    CHECK(recomp_animation_solve_skeleton(&t, p, &s, b));
    CHECK(fabsf(b[2].m[9]) > 0.01f);
    memcpy(before, b, sizeof b);
    t.limbs[0].position_channels[2] = 60;
    CHECK(!recomp_animation_solve_skeleton(&t, p, &s, b));
    CHECK(memcmp(before, b, sizeof b) == 0);
    t = saved_tables; s = saved_targets; p[14] = 0;
    CHECK(!recomp_animation_solve_skeleton(&t, p, &s, b));
    CHECK(memcmp(before, b, sizeof b) == 0);
    p[14] = NAN;
    CHECK(!recomp_animation_solve_skeleton(&t, p, &s, b));
    CHECK(memcmp(before, b, sizeof b) == 0);
    puts("Animation skeleton tests passed");
    return 0;
}
