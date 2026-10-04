# D3D8 fog in the D3D11 presenter

Fog follows the game's enable, color, mode, start, end, density and range
states. It is on by default. `RECOMP_D3D_FOG=0` disables host fog for A/B
comparison. `RECOMP_D3D_FOG_FACTOR=1` displays the factor on fog-enabled
draws (white is clear, black is fully fogged).

The draw adapter reads the retained Xbox render-state array, snapshots it
in the presenter command and passes constants to Shader Model 4.0 shaders.
These deferred states are not stored in the simple-method model. Generated
D3D8 setters still own this array; wholesale D3D8 state replacement remains
open work. No generated source is changed.

## Semantics

Xbox exposes `FOGTABLEMODE`, not a separate PC `FOGVERTEXMODE` state.
Mode NONE uses the vertex factor: specular alpha for fixed-function vertices,
including XYZRHW, or the admitted vertex program's fog output. An absent
specular component defaults to no fog. The factor interpolates in screen
space. EXP, EXP2 and LINEAR evaluate the interpolated fog coordinate in the
pixel shader and clamp the resulting factor to [0, 1].

Perspective projection uses clip W (eye Z for a conventional perspective
matrix). XYZRHW reconstructs W from reciprocal W. An affine projection with
last column (0, 0, 0, 1) selects absolute projected Z instead. Range fog uses
the length of the transformed eye position, including vertex blending;
pretransformed vertices retain their Z/W path because they have no eye
position. Programmable vertices supply their fog coordinate directly.

These choices follow the public Xbox state definitions and coordinate
selection in [Cxbx's D3D8 types](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/XbD3D8Types.h)
and [fixed-function state setup](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/Direct3D9/Direct3D9.cpp).
The implementation is independent. The coordinate behavior is also described
in Microsoft's [pixel fog](https://learn.microsoft.com/en-us/windows/win32/direct3d9/pixel-fog)
and [vertex fog](https://learn.microsoft.com/en-us/windows/win32/direct3d9/vertex-fog)
documentation. This is API-level fog, not an NV2A table emulator.

Fog changes RGB after the texture-stage result and before framebuffer
blending; alpha is preserved. Additive draws still fog toward FOGCOLOR.
Applications must request black themselves when needed for additive layers.
Fog-disabled draws bypass the operation. Equal linear start/end values use
a step at end to avoid division by zero.

The render-state test checks float-bit decoding. The D3D11 readback test
checks all three equations, RHW versus Z, specular-alpha vertex fog,
disabled fog, alpha preservation and fog color before additive blending.
Actual scene comparisons and performance receipts belong under `private/`.
