#ifndef DOAXBV_RECOMP_FREE_CAMERA_H
#define DOAXBV_RECOMP_FREE_CAMERA_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A fly camera in the game's world space, which is right-handed: +Y up, and
   looking down +Z, +X is on the left. A heading of zero looks down +Z. It knows nothing of the guest; free_camera_adapter.c feeds it from
   and to the game's camera objects. */
typedef struct RecompFreeCamera {
    float position[3];
    /* Radians. Heading grows turning left, toward +X; pitch is positive
       looking up. */
    float heading, pitch;
    /* Vertical field of view in radians. */
    float fov;
    /* World units per second at full stick, scaled from the distance between
       the game's eye and target when the camera starts. */
    float speed;
    /* The game's eye-to-target distance when the camera started; the target
       stays that far ahead, since the game may read it as a focus distance. */
    float focus;
} RecompFreeCamera;

/* One frame of controls, each axis in [-1, 1]. */
typedef struct RecompFreeCameraControls {
    /* Right, up, forward. */
    float move[3];
    /* Heading (positive turns right) and pitch (positive looks up). */
    float look[2];
    /* Radians to turn at once, in the same senses as look: a mouse drag. */
    float turn[2];
    /* Positive narrows the field of view. */
    float zoom;
    bool fast, slow;
} RecompFreeCameraControls;

enum {
    RECOMP_FREE_CAMERA_FAST_SCALE = 4,
};

/* Starts the camera where the game's camera is. False leaves it untouched
   when eye and target coincide or are not finite. */
bool recomp_free_camera_begin(RecompFreeCamera *camera,
    const float eye[3], const float target[3], float fov);
void recomp_free_camera_step(RecompFreeCamera *camera,
    const RecompFreeCameraControls *controls, float seconds);
/* Turns the camera to face a point, keeping its position. False leaves it
   untouched when the point is the camera's position or not finite. */
bool recomp_free_camera_look_at(RecompFreeCamera *camera, const float point[3]);
/* Moves the camera to the given distance from a point, along the line it
   now sees it on, and faces it with the given field of view. */
bool recomp_free_camera_frame(RecompFreeCamera *camera, const float point[3],
    float distance, float fov);
/* The point the camera looks at, focus units ahead of the position. */
void recomp_free_camera_target(const RecompFreeCamera *camera, float target[3]);

#ifdef __cplusplus
}
#endif

#endif
