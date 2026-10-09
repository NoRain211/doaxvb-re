#ifndef DOAXBV_ANIMATION_CAMERA_H
#define DOAXBV_ANIMATION_CAMERA_H
#include <stdbool.h>

typedef struct RecompVisualCamera {
    float eye[3], target[3], roll, fov, near_z, far_z;
    float bias_x, bias_y, scale_x, scale_y, scale_z, aspect;
} RecompVisualCamera;

#ifdef __cplusplus
extern "C" {
#endif
/* Rebuilds the row-vector view/projection from immutable visual camera inputs. */
bool recomp_animation_camera(const RecompVisualCamera *input, float view[16], float projection[16]);
bool recomp_animation_camera_sample(const RecompVisualCamera inputs[2], float fraction,
    float view[16], float projection[16]);
#ifdef __cplusplus
}
#endif
#endif
