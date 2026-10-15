#include "free_camera.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(bool condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "free camera test failed: %s\n", what);
        ++failures;
    }
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 1e-4f;
}

int main(void)
{
    RecompFreeCamera camera;
    RecompFreeCameraControls controls;
    float target[3];

    const float eye[3] = {1.0f, 2.0f, 3.0f};
    const float ahead[3] = {1.0f, 2.0f, 13.0f};
    check(recomp_free_camera_begin(&camera, eye, ahead, 0.8f), "begin from a level view");
    check(near(camera.heading, 0.0f) && near(camera.pitch, 0.0f), "level view down +Z");
    check(near(camera.focus, 10.0f) && near(camera.speed, 5.0f), "focus and speed from the gap");
    recomp_free_camera_target(&camera, target);
    check(near(target[0], 1.0f) && near(target[1], 2.0f) && near(target[2], 13.0f),
        "target reproduces the game's target");

    memset(&controls, 0, sizeof controls);
    controls.move[2] = 1.0f;
    recomp_free_camera_step(&camera, &controls, 1.0f);
    check(near(camera.position[2], 8.0f), "forward moves speed units a second");

    memset(&controls, 0, sizeof controls);
    controls.move[0] = 1.0f;
    controls.fast = true;
    recomp_free_camera_step(&camera, &controls, 0.5f);
    check(near(camera.position[0], -9.0f), "right strafes along -X at a zero heading, fast x4");

    memset(&controls, 0, sizeof controls);
    controls.look[0] = 1.0f;
    recomp_free_camera_step(&camera, &controls, 0.1f);
    check(camera.heading < 0.0f, "looking right turns the heading negative");
    recomp_free_camera_target(&camera, target);
    check(target[0] < camera.position[0], "looking right turns toward -X, the right-hand side");

    memset(&controls, 0, sizeof controls);
    float heading = camera.heading;
    controls.turn[0] = 0.25f;
    recomp_free_camera_step(&camera, &controls, 0.1f);
    check(near(camera.heading, heading - 0.25f), "a turn applies its radians at once");
    controls.turn[0] = NAN;
    recomp_free_camera_step(&camera, &controls, 0.1f);
    check(near(camera.heading, heading - 0.25f), "a non-finite turn is ignored");

    memset(&controls, 0, sizeof controls);
    controls.look[1] = 1.0f;
    for (int i = 0; i < 200; ++i) recomp_free_camera_step(&camera, &controls, 0.1f);
    check(camera.pitch < 1.5708f && camera.pitch > 1.5f, "pitch stops short of straight up");

    memset(&controls, 0, sizeof controls);
    controls.zoom = 1.0f;
    float before = camera.fov;
    recomp_free_camera_step(&camera, &controls, 0.5f);
    check(camera.fov < before, "zooming in narrows the field of view");

    const float origin[3] = {0.0f, 0.0f, 0.0f};
    const float to_left[3] = {5.0f, 0.0f, 0.0f};
    check(recomp_free_camera_begin(&camera, origin, ahead, 0.8f), "begin at the origin");
    check(recomp_free_camera_look_at(&camera, to_left), "look at a point");
    recomp_free_camera_target(&camera, target);
    check(near(camera.position[0], 0.0f) && target[0] > 9.9f && fabsf(target[2]) < 1e-3f,
        "looking at a point keeps the position and faces it");
    check(!recomp_free_camera_look_at(&camera, origin), "looking at the camera's own position is refused");

    const float far_point[3] = {0.0f, 0.0f, 20.0f};
    check(recomp_free_camera_begin(&camera, origin, ahead, 0.2f), "begin for framing");
    check(recomp_free_camera_frame(&camera, far_point, 2.5f, 0.8f), "frame a far point");
    check(near(camera.position[2], 17.5f) && near(camera.fov, 0.8f) && near(camera.focus, 2.5f),
        "framing moves to the distance along the line of sight");

    RecompFreeCamera untouched = camera;
    check(!recomp_free_camera_begin(&camera, eye, eye, 0.8f), "eye on target is refused");
    check(memcmp(&untouched, &camera, sizeof camera) == 0, "a refused begin leaves the camera");
    const float nan_target[3] = {NAN, 0.0f, 0.0f};
    check(!recomp_free_camera_begin(&camera, eye, nan_target, 0.8f), "a non-finite target is refused");

    const float above[3] = {1.0f, 12.0f, 3.0f};
    check(recomp_free_camera_begin(&camera, eye, above, 0.8f), "begin looking straight up");
    check(camera.pitch < 1.5708f, "a vertical view is clamped below straight up");

    if (failures == 0) printf("free camera test passed\n");
    return failures == 0 ? 0 : 1;
}
