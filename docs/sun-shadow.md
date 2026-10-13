# Experimental directional sun shadows

Set `RECOMP_D3D_SUN_SHADOW=1` before launching the D3D11 runner. Unset, `0`,
and other values retain the existing shaders, depth allocation and draw path;
no shadow GPU resources or prepasses are created. This is a visual experiment,
not a calibrated replacement for the game's lighting.

The render worker runs camera selection and a caster pass through the same
pose/split replay as the visible pass. Camera selection accepts finite,
invertible view-projection matrices from main-target, transformed,
normal-bearing fixed-function draws without vertex diffuse. It requires depth
test/write and host lighting, rejects world extents of 200 units or more,
and chooses the largest triangle count. This excludes the observed sky domes
and cloud sheets without relying on draw order or private texture identities.
Selection resets each frame and at main-target depth clears. Only the selected
depth interval and matching camera receive shadows.

A single 2048-square D32 shadow map covers a 48-unit box centered ten units
along the camera's near ray, with 120 units of light-space depth. The XY fit
snaps to shadow texels. Casters are opaque (including alpha-tested), host-lit
normal-bearing skinned draws and rigid draws no larger than four world units.
The rigid rule includes the ball but can include small scenery; transparent
hair and distant objects are outside this initial test. Vertex blending uses
the command's replayed `world[]` palette and the same explicit/remainder
weights as the visible pass.

The sun is the stage's key light: slot 0 of the background light rig, which
the game saves each frame (four 0x58-byte rows at guest 0x009D4F58; kind at +0,
elevation and azimuth at +0x24 and +0x28 in 1/65536 turns). The draw adapter
reads it into each draw's `sun` as a world-space toward-light vector, valid only
when slot 0 is directional. When it is not, no shadow map is built and the
game's own shadows stay. Slot 0 is a key-light proxy: on the sunset court it
sits about 14 degrees from the sun sprite, which the game places separately.

`sunDirection()` picks the receiver's directional light nearest that key light,
within 25 degrees, and attenuates only that light. Matching by direction stands
in for slot provenance. `RECOMP_D3D_SUN_DIR="x,y,z"` overrides the map's **light
travel** direction, normalized on input; invalid, zero and nonfinite values are
ignored. It does not move the sky sun or change the game's light rig.

Nine comparison-filtered samples provide shadow visibility. Only the selected
direct diffuse term is reduced before material/texture processing and fog;
ambient, point lights and other directional terms remain. While the shadow map is valid, the game's projected shadow for characters and
the ball is skipped. Only its darkening pass is matched: FVF 0x112, untextured,
lighting off, black texture factor with any alpha, stencil EQUAL 1 with KEEP
operations, depth writes off and SRC_ALPHA blending. The stencil-marking pass
still draws, so stencil state matches the original frame. Other non-host-lit
passes, including the existing projected palm/net shadow receivers, bypass the effect.
The implementation uses ordinary D3D11 depth comparisons and slope bias;
see Microsoft's [comparison sampling documentation](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/samplecmplevelzero-s-float-float-int-uint-)
and [depth bias documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage-depth-bias).

The scene depth stays D24S8, including under MSAA. No readable scene-depth SRV
is added because the shadow test does not sample it. Shadow resources release
with the presenter; partial creation failure releases them and disables the
feature. The map is unbound as an SRV before depth writes. D3D11 remains the
backend and no vendor-specific capability is required.

The presenter tests cover opt-out resource parity, invalid cameras and sky
extent rejection, sun selection/override validation, GPU shadow visibility,
ambient/fill preservation, rigid and one-to-three-weight casters, bypasses and
resource release. Private smoke captures establish startup and frame delivery
only. Visual acceptance, bias tuning and alignment with the visible sun require
the user's play test. `RECOMP_PERF_COUNTER=1` reports caster counts and the map's
direction alongside the existing FPS/late-frame observations.
