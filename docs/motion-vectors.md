# Motion vectors and jitter (D3D11)

Inputs for a temporal upscaler such as DLSS. The game's fixed-function
renderer supplies neither, so the presenter derives them. Off by default.

| Variable | Effect |
| --- | --- |
| `RECOMP_D3D_MOTION_VECTORS=1` | Compute camera motion vectors each frame |
| `RECOMP_D3D_JITTER=1` | Offset 3D scene draws by a sub-pixel Halton (2, 3) pattern, 8 phases; implies motion vectors |
| `RECOMP_D3D_MOTION_DEBUG=1` | Blend the vectors over the frame: grey is still, hue is direction, brightness is length (16 px saturates); implies motion vectors |
| `RECOMP_D3D_JITTER_SCALE=<n>` | Test only: multiplies the jitter so a frame capture shows what moves |

Jitter on its own makes the picture shimmer; it is meant for an upscaler that
resolves it.

## What counts as the 3D scene

A draw into the main colour and depth target, depth-tested, not pretransformed
and not a sun-shadow pass. Those draws take the jitter through `taa_jitter` in
the draw constant buffer, applied in clip space by both the fixed-function and
the vertex-program shaders. Screen-space (pretransformed) draws, such as the
HUD, and offscreen targets get no jitter. The scene draw with the most indices
names the frame's camera (`view_projection`, the game's view times projection),
as RTGI does.

## Camera vectors

After the 3D scene, at the first screen-space draw on the main target or at
present when there is none, a full-screen pass reads the main depth buffer,
removes the jitter, unprojects with the inverse of this frame's camera and
projects with the previous frame's. The result is an `R16G16_FLOAT` texture at
the render resolution, in render pixels, from the current to the previous
position (the DLSS convention). The first frame, or one without a scene
camera, reports zero.

## Object vectors

Fixed-function scene draws also place each vertex with last frame's matrices.
Each draw records its world matrices (all four for skinned draws) and its
view-projection, keyed by mesh: vertex address, index and vertex counts and
FVF. Next frame a draw of the same mesh takes the recorded copy nearest to it
in world position, since the game re-sorts repeated meshes such as foliage by
distance and draw order alone paired the wrong copies. `previous_wvp[]` and
`motion_flags` in the draw constants carry them to the shader, which writes the
pixel difference and the fragment depth to `SV_Target1`, an
`R32G32B32A32_FLOAT` target at the scene size, which keeps the depth exact
enough to match the depth buffer. Only opaque, depth-writing draws
write it.

The motion pass then uses an object's own vector wherever that object is still
the visible surface (its stored depth matches the depth buffer) and the camera
vector everywhere else. Vertex-program draws, blended surfaces and meshes not
seen last frame keep camera vectors. Object vectors need single-sampled
targets and share `SV_Target1` with the G-buffer, so they are off with MSAA or
`RECOMP_D3D_GBUFFER`.

Movement the game makes by rewriting vertices on the CPU rather than through
matrices is not captured; the debug view showed the girls' limbs moving through
their matrices.

## Observed (2026-11-01)

CPU Exhibition on court 2: a still camera shows uniform grey; a moving
camera shows smooth gradients with breaks at object silhouettes, the far
jungle and near posts moving in opposite directions as the camera orbits the
court. With `RECOMP_D3D_JITTER_SCALE=40` consecutive captures show the scene
offset by tens of pixels.

Object vectors, DLSS Quality, court 2: the girls and the ball show their own
motion against the background, a waving hand brighter than the body; palm
fronds stay consistent with their surroundings after matching repeated meshes
by position (draw-order matching striped their edges).
