#include "animation_visual.h"

#include <math.h>
#include <string.h>

bool recomp_animation_visual_sample(const RecompVisualPose *input, float fraction,
    RecompBoneMatrix output[32])
{
    if (!input || !output || !isfinite(fraction) || fraction < 0 || fraction > 1) return false;
    if (fraction == 0 || fraction == 1) {
        memcpy(output, input->endpoints[fraction == 1], sizeof input->endpoints[0]);
        return true;
    }
    RecompAnimationPose pose = input->channels[0];
    if (!recomp_animation_pose_blend(input->groups, 24, input->channels, input->channels+1,
            fraction, &pose)) return false;
    RecompSkeletonTargets targets = input->targets[0];
    for (unsigned i = 0; i < 3; ++i)
        targets.position[i] = (float)(targets.position[i]+
            ((double)input->targets[1].position[i]-targets.position[i])*fraction);
    const double pi = 3.14159265358979323846;
    double angle = fmod((double)input->targets[1].heading-targets.heading+pi, 2*pi);
    if (angle < 0) angle += 2*pi;
    targets.heading = (float)(targets.heading+(angle-pi)*fraction);
    return recomp_animation_solve_skeleton(&input->tables, pose.channels, &targets, output);
}
