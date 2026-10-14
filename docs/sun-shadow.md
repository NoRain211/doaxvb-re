# Experimental directional sun shadows

Set `RECOMP_D3D_SUN_SHADOW=1` before launching the D3D11 runner, or tick **Sun
shadows** in `Launcher.cmd`; leave it off on lower-powered PCs. Unset, `0`,
and other values retain the existing shaders, depth allocation and draw path;
no shadow GPU resources or prepasses are created. This is a visual experiment,
not a calibrated replacement for the game's lighting.

The render worker runs camera selection and a caster pass through the same
pose/split replay as the visible pass. Camera selection accepts finite,
invertible view-projection matrices from main-target, transformed,
normal-bearing fixed-function draws without vertex diffuse. Bounds use only
referenced vertices. It requires depth
test/write and host lighting, rejects world extents of 200 units or more,
and chooses the largest triangle count. This excludes the observed sky domes
and cloud sheets without relying on draw order or private texture identities.
Selection resets each frame and at main-target depth clears. Only the selected
depth interval and matching camera receive shadows.

A single 4096-square D32 shadow map covers a 64-unit box centered 20 units
along the camera's near ray, with 120 units of light-space depth. The XY fit
snaps to shadow texels and shadows fade over the outer tenth of the box.
Receivers offset their lookup 0.04 units along the vertex normal, and the
compare bias is 0.0002 of light depth, so grazing ground under a low sun does
not shadow itself. Skinned draws and rigid casters (the ball and scene props) do
not receive, so models keep the game's shading. Other receivers darken by the
game's own shadow opacity, taken from last frame's hidden darkening pass,
because many stages light the sand with a separate rig or mostly baked light.
When the game drew no shadow last frame (hotel, menus), nothing receives.
The caster pass records rigid models in draw order; the visible pass reads
that flag by its own draw count. Casters retain the opaque host-lit skinned
and compact rigid draws, and add textured posts, palms and net geometry,
including unlit draws and vertex colors. Narrow opaque props have at most
eight units of horizontal extent and at most one unit across their narrower
horizontal axis. Upright cutouts can span 128 units to accommodate batched
trees; opaque cutouts require a GREATER/GREATER_EQUAL alpha reference of at
least 128. Their height must be at least 5% of horizontal extent, or 10% for
SRC_ALPHA/INV_SRC_ALPHA blended cutouts. This rejects flat ground and shadow
overlays, broad opaque rocks, sea and sky. These are geometry/state limits,
not an object identity classifier.

The caster pass samples base-mip texture alpha independently of material or
vertex alpha, so mip averages do not erase thin net wires. Opaque cutouts keep the game's alpha comparison and reference;
blended cutouts use GREATER 127 and write map depth even when the visible draw
does not write depth. Render-target textures and additive effects are excluded
from the added props. The fixed map fit does not expand for larger casters.
Vertex blending uses the command's replayed `world[]` palette and the same
explicit/remainder weights as the visible pass.

The light direction is the one the game projects its own character and ball
shadows along: three floats at guest 0x00333870, a toward-light vector that the
scene setters normalize. The draw adapter copies it into each draw's `sun`. The
stage key light (background rig slot 0 at 0x009D4F58) differs by up to about 20
degrees and points upward on some daytime stages, so it is not used.

`sunDirection()` accepts a host-lit camera-defining draw when that direction
is above the horizon; otherwise no shadow map is built and the game's own
shadows stay. `RECOMP_D3D_SUN_DIR="x,y,z"` overrides the map's **light
travel** direction, normalized on input; invalid, zero and nonfinite values are
ignored. It does not move the sky sun or change the game's light rig.

Nine comparison-filtered samples provide shadow visibility near the camera.
When a screen pixel covers more than three shadow texels, a 4x4 filter covers
its footprint (capped at sixteen texels) so fine net wires do not pulse as the
camera moves. Receiver-plane depth correction keeps that wider filter from
shadowing sloped ground. Visibility scales the receiver's lit vertex color
before material/texture processing and fog. While the shadow map is valid, the game's projected shadow for characters and
the ball is skipped. Only its darkening pass is matched: FVF 0x112, untextured,
lighting off, black texture factor with any alpha, stencil EQUAL 1 then ZERO,
depth writes off and SRC_ALPHA blending. It still draws with color writes off,
as does the stencil-marking pass, so stencil state matches the original frame.
Verified post and palm overlays also keep only depth/stencil writes: their
unlit alpha-blended receiver states must match along with texture identity.
Static DXT5 overlays use full compressed-mip payload hashes cached on upload.
Live palm targets must be swizzled ARGB8 at 512x256 or 256x512, cleared to
transparent and populated only by textured black silhouette passes without
depth or stencil testing. Another color-writing producer invalidates that
identity until the next clear; releasing the target retires it. Unknown
payloads and targets keep drawing. Suppression requires a valid sun map.
The caster rules above provide replacements within the map coverage; objects
outside that coverage do not cast into the visible map.
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
ground darkening by the game's shadow opacity, compact and no-game-shadow
bypasses, rigid and one-to-three-weight casters, referenced bounds, prop/receiver
selection, texture-alpha cutouts under 1x/8x MSAA and resource release. Private
attract-demo captures matched only whole actor and ball shadow sets (19 passes
per actor) across seven scenes. A user play test accepted the look on the
morning, dusk, sunset, Niki Beach and private beach courts and measured 59.9 fps
after startup at 2160p with 8x MSAA and SMAA; the hotel is not verified.
`RECOMP_PERF_COUNTER=1` reports caster counts, hidden game-shadow passes,
hidden prop overlays (`prop_shadows`) and the
map's direction alongside the existing FPS/late-frame observations.
