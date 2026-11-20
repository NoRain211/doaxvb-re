#ifndef DOAXBV_RECOMP_D3D_MOTION_H
#define DOAXBV_RECOMP_D3D_MOTION_H

/* Motion vectors for temporal upscalers (docs/motion-vectors.md), in render
   pixels from the current to the previous position (the DLSS convention).
   Camera motion reprojects scene depth with last frame's camera; object
   motion comes from fixed-function draws that also place each vertex with
   last frame's matrices. Every call runs on the render worker thread that
   owns the D3D11 immediate context. */

#include "d3d_presenter.h"

#include <d3d11.h>

#include <cstdint>
#include <string>

struct D3dMotion;

/* width x height is the 3D scene's size. multisampled says the depth view is
   a Texture2DMS. objects adds the object vector target, which needs
   single-sampled targets and a free SV_Target1; if that target fails, the
   motion logs it and keeps camera vectors only. Returns null after logging why. */
D3dMotion *d3d_motion_create(ID3D11Device *device, ID3D11DeviceContext *context,
    uint32_t width, uint32_t height, bool multisampled, bool objects);
void d3d_motion_destroy(D3dMotion *motion);

// The R16G16_FLOAT vector target a temporal upscaler reads.
ID3D11RenderTargetView *d3d_motion_target(D3dMotion *motion);
// SV_Target1 for scene draws, or null without object vectors or a null motion.
ID3D11RenderTargetView *d3d_motion_object_target(D3dMotion *motion);

/* Writes the frame's vectors from depth, the scene depth view. camera is this
   frame's view-projection, or null for a frame without one; previous is last
   frame's; jitter is the clip-space offset baked into depth. A null or
   singular camera writes zero motion. Leaves the context state cleared. */
void d3d_motion_reproject(D3dMotion *motion, ID3D11DeviceContext *context,
    ID3D11ShaderResourceView *depth, const float *camera, const float *previous,
    const float jitter[2]);

// Blends the vectors as colour over output, width x height pixels.
void d3d_motion_draw_debug(D3dMotion *motion, ID3D11DeviceContext *context,
    ID3D11RenderTargetView *output, uint32_t width, uint32_t height);

/* For a fixed-function scene draw: writes last frame's world x view-projection
   of the same mesh, its copy nearest in world position, to previous_wvp (16
   floats per matrix) and records this draw for the next frame. Returns false
   without object vectors, for draws they skip, or without a match. */
bool d3d_motion_object(D3dMotion *motion, const RecompD3dPresenterDrawCommand &draw,
    float *previous_wvp);

// After a present: this frame's objects become last frame's.
void d3d_motion_end_frame(D3dMotion *motion, ID3D11DeviceContext *context);

/* Edits a draw shader so it writes object motion to SV_Target1 when
   motion_flags.x is set. A source without the expected anchors is left
   unchanged and its draws keep camera vectors. */
void d3d_motion_shader_source(std::string &source);

// Halton low-discrepancy sequence, the jitter pattern temporal upscalers expect.
float d3d_motion_halton(uint32_t index, uint32_t base);

#endif
