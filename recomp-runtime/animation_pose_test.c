#include "animation_pose.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Pose line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
    const RecompAnimationGroup groups[] = {
        {1,0,RECOMP_ANIMATION_TRANSLATION,3},
        {2,6,RECOMP_ANIMATION_EULER,3},
        {4,54,RECOMP_ANIMATION_WRAPPED_ANGLE,1},
        {8,18,RECOMP_ANIMATION_DIRECTION,2},
        {16,56,RECOMP_ANIMATION_EYE_DIRECTION,2},
        {32,50,RECOMP_ANIMATION_SHOULDER_DIRECTION,2}
    };
    RecompAnimationPose a = {0}, b = {0}, out = {0};
    a.mask = b.mask = 63;
    out.mask = 123;
    a.channels[0] = -1; b.channels[0] = 3;
    b.channels[7] = 1.57079632679f;
    a.channels[54] = 3.05f; b.channels[54] = -3.05f;
    b.channels[19] = b.channels[51] = 1;
    a.channels[56] = b.channels[56] = 0.1234567f;
    a.channels[57] = b.channels[57] = 0.3456789f;
    RecompAnimationPose saved_a = a, saved_b = b;
    CHECK(recomp_animation_pose_blend(groups, 6, &a, &b, 0.5f, &out));
    CHECK(out.channels[0] == 1);
    CHECK(fabsf(out.channels[7]-0.78539816339f) < 0.001f);
    CHECK(fabsf(out.channels[54]-3.14159265359f) < 0.000001f);
    CHECK(fabsf(out.channels[19]-0.5f) < 0.001f);
    CHECK(fabsf(out.channels[51]-0.5f) < 0.001f);
    CHECK(out.channels[56] == a.channels[56] && out.channels[57] == a.channels[57]);
    CHECK(out.mask == 123 && out.tick == 0 && out.flags == 0);
    CHECK(memcmp(&a, &saved_a, sizeof a) == 0 && memcmp(&b, &saved_b, sizeof b) == 0);
    b.mask &= ~1u;
    out.channels[10] = 99;
    CHECK(recomp_animation_pose_blend(groups, 6, &a, &b, 0.5f, &out));
    CHECK(out.channels[0] == a.channels[0] && out.channels[10] == 99);
    a.mask = b.mask = 0;
    out.channels[3] = -0.2f;
    CHECK(recomp_animation_pose_blend(groups, 6, &a, &b, 0.5f, &out));
    CHECK(out.channels[3] > 6.0f && out.channels[3] < 6.1f);
    RecompAnimationPose before = out;
    CHECK(!recomp_animation_pose_blend(groups, 6, &a, &b, NAN, &out));
    CHECK(memcmp(&out, &before, sizeof out) == 0);
    RecompAnimationGroup invalid = {1,59,RECOMP_ANIMATION_EULER,3};
    CHECK(!recomp_animation_pose_blend(&invalid, 1, &a, &b, 0.5f, &out));
    CHECK(memcmp(&out, &before, sizeof out) == 0);
    puts("Animation pose blend tests passed");
    return 0;
}
