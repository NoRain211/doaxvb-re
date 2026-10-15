#include "free_camera.h"

#include <math.h>
#include <stddef.h>

enum { AXES = 3 };

static const float PI = 3.14159265358979f;
/* Radians per second at full stick. */
static const float TURN_RATE = 1.8f;
/* Just short of straight up or down, where the heading would be lost. */
static const float PITCH_LIMIT = 1.55f;
static const float FOV_MIN = 0.05f;
static const float FOV_MAX = 2.6f;
static const float ZOOM_RATE = 1.2f;
static const float SPEED_MIN = 0.01f;

static float clampf(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

static void forward_of(const RecompFreeCamera *camera, float forward[AXES])
{
    float level = cosf(camera->pitch);
    forward[0] = sinf(camera->heading) * level;
    forward[1] = sinf(camera->pitch);
    forward[2] = cosf(camera->heading) * level;
}

bool recomp_free_camera_begin(RecompFreeCamera *camera,
    const float eye[3], const float target[3], float fov)
{
    float view[AXES];
    float squared = 0.0f;

    if (camera == NULL || eye == NULL || target == NULL) {
        return false;
    }
    for (unsigned i = 0u; i < AXES; ++i) {
        if (!isfinite(eye[i]) || !isfinite(target[i])) {
            return false;
        }
        view[i] = target[i] - eye[i];
        squared += view[i] * view[i];
    }
    if (!(squared > 0.0f) || !isfinite(squared)) {
        return false;
    }
    float distance = sqrtf(squared);
    for (unsigned i = 0u; i < AXES; ++i) {
        camera->position[i] = eye[i];
    }
    camera->heading = atan2f(view[0], view[2]);
    camera->pitch = clampf(asinf(clampf(view[1] / distance, -1.0f, 1.0f)),
        -PITCH_LIMIT, PITCH_LIMIT);
    camera->fov = isfinite(fov) ? clampf(fov, FOV_MIN, FOV_MAX) : 1.0f;
    /* Crossing the gap to the target in about two seconds suits both a
       close-up and a wide shot. */
    camera->speed = distance * 0.5f > SPEED_MIN ? distance * 0.5f : SPEED_MIN;
    camera->focus = distance;
    return true;
}

void recomp_free_camera_step(RecompFreeCamera *camera,
    const RecompFreeCameraControls *controls, float seconds)
{
    if (camera == NULL || controls == NULL || !(seconds > 0.0f)) {
        return;
    }
    float scale = controls->fast ? (float)RECOMP_FREE_CAMERA_FAST_SCALE : 1.0f;
    if (controls->slow) {
        scale /= (float)RECOMP_FREE_CAMERA_FAST_SCALE;
    }

    /* A narrow field of view turns slower so aiming stays steady. */
    float turn = TURN_RATE * seconds * (camera->fov / 1.0f);
    camera->heading -= clampf(controls->look[0], -1.0f, 1.0f) * turn +
        (isfinite(controls->turn[0]) ? controls->turn[0] : 0.0f);
    camera->heading = remainderf(camera->heading, 2.0f * PI);
    camera->pitch = clampf(camera->pitch + clampf(controls->look[1], -1.0f, 1.0f) * turn +
        (isfinite(controls->turn[1]) ? controls->turn[1] : 0.0f),
        -PITCH_LIMIT, PITCH_LIMIT);
    camera->fov = clampf(
        camera->fov * expf(-clampf(controls->zoom, -1.0f, 1.0f) * ZOOM_RATE * seconds),
        FOV_MIN, FOV_MAX);

    float forward[AXES];
    forward_of(camera, forward);
    /* Strafing stays level: right is the heading turned a quarter, and up is
       world up, so the stick moves along the ground whatever the pitch. */
    const float right[AXES] = {-cosf(camera->heading), 0.0f, sinf(camera->heading)};
    const float up[AXES] = {0.0f, 1.0f, 0.0f};
    float step = camera->speed * scale * seconds;
    for (unsigned i = 0u; i < AXES; ++i) {
        camera->position[i] += step * (
            clampf(controls->move[0], -1.0f, 1.0f) * right[i] +
            clampf(controls->move[1], -1.0f, 1.0f) * up[i] +
            clampf(controls->move[2], -1.0f, 1.0f) * forward[i]);
    }
}

bool recomp_free_camera_look_at(RecompFreeCamera *camera, const float point[3])
{
    float view[AXES];
    float squared = 0.0f;

    if (camera == NULL || point == NULL) return false;
    for (unsigned i = 0u; i < AXES; ++i) {
        if (!isfinite(point[i])) return false;
        view[i] = point[i] - camera->position[i];
        squared += view[i] * view[i];
    }
    if (!(squared > 0.0f) || !isfinite(squared)) return false;
    float distance = sqrtf(squared);
    camera->heading = atan2f(view[0], view[2]);
    camera->pitch = clampf(asinf(clampf(view[1] / distance, -1.0f, 1.0f)),
        -PITCH_LIMIT, PITCH_LIMIT);
    return true;
}

bool recomp_free_camera_frame(RecompFreeCamera *camera, const float point[3],
    float distance, float fov)
{
    float view[AXES];
    float squared = 0.0f;

    if (camera == NULL || point == NULL || !(distance > 0.0f) || !isfinite(distance)) return false;
    for (unsigned i = 0u; i < AXES; ++i) {
        if (!isfinite(point[i])) return false;
        view[i] = point[i] - camera->position[i];
        squared += view[i] * view[i];
    }
    if (!(squared > 0.0f) || !isfinite(squared)) return false;
    float length = sqrtf(squared);
    for (unsigned i = 0u; i < AXES; ++i) {
        camera->position[i] = point[i] - view[i] / length * distance;
    }
    camera->fov = isfinite(fov) ? clampf(fov, FOV_MIN, FOV_MAX) : camera->fov;
    camera->focus = distance;
    camera->speed = distance * 0.5f > SPEED_MIN ? distance * 0.5f : SPEED_MIN;
    return recomp_free_camera_look_at(camera, point);
}

void recomp_free_camera_target(const RecompFreeCamera *camera, float target[3])
{
    float forward[AXES];

    forward_of(camera, forward);
    for (unsigned i = 0u; i < AXES; ++i) {
        target[i] = camera->position[i] + forward[i] * camera->focus;
    }
}
