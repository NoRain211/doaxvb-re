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
Lit receivers offset their lookup along the per-pixel normal, 0.04 units facing
the sun and up to 0.07 at grazing angles, and the compare bias is 0.0002 of
light depth, so grazing ground under a low sun does not shadow itself. Lit
skinned characters receive too, but per pixel the shadow fades out where the
surface turns from the sun (N dot L below 0.3, gone below 0.05): the game's
shading already darkens those sides, and the map there only made self-shadow
blotches. Rigid casters (the ball and scene props) do not receive and keep the
Characters also use a filter at least three map texels wide and 60% of the
ground strength, because close-ups magnify map texels into hard facets.
game's shading. Other receivers darken by the
game's own shadow opacity, taken from last frame's hidden darkening pass,
because many stages light the sand with a separate rig or mostly baked light.
When the game drew no shadow last frame (hotel, menus), nothing receives.
Unlit opaque or alpha-blended depth-writing draws also receive, so shadows
cross from the sand onto the baked-color grass verge instead of stopping at its
edge. Their world position comes from the draw's world matrix.
The caster pass records rigid models in draw order; the visible pass reads
that flag by its own draw count. Casters retain the opaque host-lit skinned
and compact rigid draws, and add textured palms and other props,
including unlit draws and vertex colors. Narrow opaque props have at most
eight units of horizontal extent and at most one unit across their narrower
horizontal axis. Upright cutouts can span 128 units to accommodate batched
trees; opaque cutouts require a GREATER/GREATER_EQUAL alpha reference of at
least 128. Their height must be at least 5% of horizontal extent, or 10% for
SRC_ALPHA/INV_SRC_ALPHA blended cutouts. This rejects flat ground and shadow
overlays, broad opaque rocks, sea and sky. Blended sheets thinner than 2% of
their width and wider than twice their height, such as the net mesh, do not
cast: their wires are finer than a map texel and reach the sand as a smear.
These are geometry/state limits,
not an object identity classifier.

The caster pass samples base-mip texture alpha independently of material or
vertex alpha, so mip averages do not erase thin leaf edges. Opaque cutouts keep the game's alpha comparison and reference;
blended cutouts use GREATER 127 and write map depth even when the visible draw
does not write depth. Render-target textures and additive effects are excluded
from the added props. The fixed map fit does not expand for larger casters.
Vertex blending uses the command's replayed `world[]` palette and the same
explicit/remainder weights as the visible pass.

The game draws only scenery whose bounding sphere passes its camera test at
0x0017D2A0, so palms and posts beside or behind the camera vanished from the
map while their shadows were still on screen. With sun shadows on, the
runtime routes that test through `recomp_lookup_manual` and also accepts
spheres within 96 units of the camera plus their radius. Those objects are
submitted to both passes; the GPU clips them from the visible frame. The
address is a manual call target, so regenerating the program changes only the
15 functions that call it.

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

Twenty-five comparison-filtered samples provide shadow visibility. Their
spacing follows the screen pixel's footprint in the map, from 0.6 texels near
the camera to at most two texels far away, so distant thin shadows such as the
remaining thin shadows receive a wider filter at a distance. This does not guarantee temporal stability.
Receiver-plane depth correction keeps the wider spacing from shadowing sloped
ground. Visibility scales the receiver's lit vertex color
before material/texture processing and fog. While the shadow map is valid, the game's projected shadow for characters and
the ball is skipped. Only its darkening pass is matched: FVF 0x112, untextured,
lighting off, black texture factor with any alpha, stencil EQUAL 1 then ZERO,
depth writes off and SRC_ALPHA blending. It still draws with color writes off,
as does the stencil-marking pass, so stencil state matches the original frame.
Legacy tree overlays also keep only depth/stencil writes: their
unlit alpha-blended receiver states must match along with texture identity.
Each court and time of day bakes its own static tree silhouettes, so a DXT5
texture counts as a tree overlay when every visible block of its top mip has
black color endpoints and alpha that never exceeds 210. Foliage fringe masks
and the net mesh reach full alpha and keep drawing, as does dense canopy shade
(more than 70% of texels covered), whose trees do not cast into the sun map.
Strips more than twice as wide as tall are cliff reflections mirrored on the
sea and keep drawing.
The runner logs each texture
this rule matches. Net/post overlays still use full compressed-mip payload hashes
cached on upload. Live palm targets must be swizzled ARGB8 (any size), cleared to
transparent and populated only by textured black silhouette passes without
depth or stencil testing. Another color-writing producer invalidates that
identity until the next clear; releasing the target retires it. Unknown
payloads and targets keep drawing. Suppression requires a valid sun map.
At dusk the game draws the projected palm shadow lit, through a texture-coordinate
projection with material alpha. Any on-screen SRC_ALPHA-blended draw sampling a
validated palm target is therefore hidden, whatever its lighting or combiner.
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

The net keeps its original textured shadow; the posts, poles and vines cast
into the sun map. The game bakes posts and net into one overlay texture, so on
upload the three verified overlays keep alpha only in 4x4 blocks near
hand-measured net lines (top line and side tapes). Every other block gets zero
alpha. The sun caster pass excludes static, untransformed draws thinner than
0.15 units across within the net assembly bounds (world X +/-3.6, Z +/-0.4,
Y -0.5 through 4): the mesh, tapes and ropes. Posts and vine bundles are
thicker and cast normally. The baked net line follows the game's fixed light,
so its ends need not meet the posts' sun shadows. The bounds rule needs
revision if a court moves or rotates the net assembly.

Legacy shadow textures are classified as palm overlays or combined net/post
overlays in the texture cache. Both types are excluded from the sun caster
pass: decals bent over uneven sand can pass the ordinary scenery bounds test.
Only palm overlays lose color writes in the visible pass; net/post overlays
retain their original rendering. The same classification is used by both
passes, without hashing payloads again for each cast.
