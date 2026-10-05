# RTGI: sun shadows and off-screen bounce color

Research date: 2026-09-30. Source baseline: `codex/rtgi-dxr`,
`de393c9116432afd094b655c5ed4531c4adf3802`. This is a source review, not a
gameplay or GPU measurement. No runner, build, or GPU test was executed.
Source links below are relative to this document; line numbers refer to that
baseline. No retail addresses, asset names, or private capture data are included.

## Recommendation

1. **Sun: conditionally observable, not established per scene.** The presenter
   already receives up to eight directional lights on an admitted fixed-function
   path. Validate which, if any, represents the outdoor sun before using it.
   A shadow ray is straightforward; separating existing direct light and existing
   shadows is the harder part. The bump-mapped sand lead warrants inspecting
   original combiner inputs: the host currently has no general bump shader.
2. **Off-screen color: start with hit-point texture lookup.** Extend the existing
   triangle/material capture for a narrow material subset. The current cache
   contains opacity, not RGB radiance. Defer a persistent radiance cache until
   missing history or shading cost demonstrably requires one.

## 1. What reaches the lighting seam

The flow is retained D3D8 state -> draw adapter -> copied presenter command ->
render worker -> D3D11 draw -> `d3d_rtgi_add_draw`. The worker does not need to
read guest lighting state. See [draw-state attachment](../../../recomp-runtime/d3d_draw_adapter.c#L754),
[command copying](../../../recomp-runtime/d3d_presenter_capture.cpp#L34), and
[RTGI submission](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L2643).

| Route | Evidence and availability | Limitation |
| --- | --- | --- |
| `SetLight` / `LightEnable` | No dedicated setters or light model were found by `rg` over the tracked runtime. `attach_directional_lighting` reads retained device material and enabled-light records; `capture_draw` can record active lights and lighting state. This is the existing draw-side seam to the remaining generated D3D8 state path. | Source establishes the consumer, not which light calls each scene actually executes. There is no scene-wide sun object or stable light identity in the command. |
| `SetRenderState(LIGHTING)` | The adapter reads the lighting flag, color-vertex selection, two-sided lighting, global ambient and material. The host render-state model has normalize-normals and blend/depth/stencil state, but no semantic light collection or lighting-enable field. | `directional.enabled == false` does **not** prove that the scene has no lights. |
| Admitted fixed function | `RecompD3dDirectionalLighting` carries directions, diffuse RGB, material diffuse, accumulated ambient/emissive, normal transforms and normalization. The D3D11 vertex shader evaluates and saturates ambient/emissive plus Lambertian directional contributions. | Admission excludes programmable draws, texture-factor selection, pretransformed/no-normal/vertex-diffuse layouts, color-vertex and two-sided lighting, unsupported stage selection, point/spot lights and more than eight enabled lights. One unsupported light rejects the whole attachment. |
| Programmable vertex shaders | `attach_vertex_program` copies an admitted program and 192 float4 constants; the D3D11 presenter uploads them. Constants can contain lighting calculations, but have no semantic names. Some slots are explicitly rewritten for viewport conversion. | No proven sun constant exists. Identify a constant by tracing its use in normal/light dot products and color output, not by recognizing a unit vector. RTGI rejects every `program_count != 0` draw, including displaced geometry. |
| Projected character shadows | The user reports shadows on sand. Commands expose textures, transforms, blend factors, depth and full stencil operations. Existing tests explicitly cover paired shadow draws that mark stencil, then compare and clear it. | There is no `is_shadow`, receiver/caster ID, projector-light field or preserved shadow mask. A texture or blend mode alone cannot identify a shadow. Final stencil may already have been cleared. |

Code references: [command fields](../../../recomp-runtime/d3d_presenter.h#L65),
[render-state model](../../../recomp-runtime/d3d_render_state_model.h#L20),
[render-state hooks](../../../recomp-runtime/d3d_render_state_adapter.c#L308),
[lighting diagnostics](../../../recomp-runtime/d3d_draw_adapter.c#L1087),
[vertex-program capture](../../../recomp-runtime/d3d_draw_adapter.c#L1301),
[D3D11 lighting](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L1363),
and [shadow stencil test](../../../recomp-runtime/d3d_render_state_model_test.c#L158).
The draw/frame/texture models describe submission and resources, not a semantic
scene, sun, or shadow projector.

### Direction and color interpretation

The adapter already negates and normalizes the stored light direction. Its
`directions[i]` is the **world-space direction toward the light**, as confirmed by
its use against world-space transformed normals in the host shader. Do not negate
it again. Public [Cxbx types](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/XbD3D8Types.h)
alias the Xbox light structure to D3DLIGHT9; Microsoft's
[light definition](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dlight9)
defines the original direction as the direction the light points in world space.
`colors[i].rgb` is the diffuse light color, separate from material diffuse.
These are game shading values, not calibrated sunlight or HDR exposure.

Select a candidate by luminance and agreement across relevant world draws, not
the first list entry or the last draw of the frame. A bright character key/fill
light need not match the environment or projected shadows. Vertex colors and
baked textures cannot uniquely determine a sun direction or separate its color
from albedo. A confirmed geometric projector could constrain direction, but
projected pixels alone do not establish a unique light, especially for blob
shadows. Therefore **automatic extraction for every scene is unproven**.

### Bump-mapped sand lead: pixel shaders and combiners

The user reports bump mapping/pixel shading, possibly confined to sand. That is
a useful lead, not a draw identification established by this review. **No DOT3
normal-map or EMBM combiner is reproduced by the current D3D11 draw shader.**
The supported four-source combiner is a different effect.

| Runtime path | What it actually implements |
| --- | --- |
| Pixel shader state | No dedicated `SetPixelShader` / `SetPixelShaderConstant` replacement or general pixel-program payload was found in the tracked runtime. The adapter documents retained structural shader state plus effective overrides in the simple render-state model. Retaining those words does not execute them on D3D11. |
| Four-source register-combiner filter | `attach_four_tap_filter` recognizes one exact state pattern for a pretransformed four-UV layout, validates effective overrides and sampler state, and requires all four stages to alias the same texture. The host computes `0.5 * (saturate((128/255)*(t0+t1)) + saturate((128/255)*(t2+t3)))`. It reproduces that filter, not bump lighting. A mismatch rejects this special path. |
| Ordinary fixed-function layout | The host pixel shader implements texture/color selection or modulation, texture factor, separate alpha mask and a specific reflection blend. It never decodes general combiner dot products, dependent bump texture fetches or a sampled tangent-space normal. The ordinary indexed path has no blanket rejection of an active pixel shader, so an otherwise accepted draw can lose its original combiner effect and render through this simplified path. This is an omitted effect, not a deliberate bump approximation. |
| Programmable vertex layout | `attach_vertex_program` explicitly returns false when a pixel shader is active. The indexed adapter then fails layout admission; such a draw never reaches D3D11 or RTGI. Admitted programs use the narrow water/projected-texture path with the host pixel shader. |
| Reflection path | The adapter requires no active pixel shader and recognizes a fixed stage pattern. Host reflection coordinates use vertex position/normal and an environment transform, then blend two textures. There is no per-pixel bump perturbation. |

Evidence: [combiner recognition](../../../recomp-runtime/d3d_draw_adapter.c#L570),
[ordinary draw-state attachment](../../../recomp-runtime/d3d_draw_adapter.c#L808),
[pixel-shader rejection](../../../recomp-runtime/d3d_draw_adapter.c#L1347),
[indexed admission](../../../recomp-runtime/d3d_draw_adapter.c#L1442),
[host pixel shader and filter](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L173),
[reflection admission](../../../recomp-runtime/d3d_draw_adapter.c#L697), and
[raw override storage](../../../recomp-runtime/d3d_render_state_model.c#L165).
Thus the code proves a support gap; it does not prove whether a particular sand
draw is approximated, rejected, or uses a different technique in the original.

**Where to look for a light vector:** no sand-specific constant index can be
named from this source alone. First identify the operation and its operands:

| Original operation/input | Candidate state and availability at the host seam |
| --- | --- |
| Vertex-program calculation | A consumed `program_constants[k]`, including its swizzle/sign, might supply a light direction, light position or half vector. The [translator](../../../recomp-runtime/d3d_vertex_program.cpp#L11) maps the decoded constant index directly to `vc[k]`; follow its dataflow into color or texture-coordinate outputs. Slots 58/59 are rewritten viewport scale/offset here. Constants are meaningful only for admitted programmable draws; fixed-function packet copying deliberately skips those arrays. A combined vertex/pixel bump program needs inspection before admission rejects it. |
| Pixel-combiner constant | Follow the dot-product operand to per-stage `PSConstant0` / `PSConstant1`, their constant mappings, or a preceding intermediate result. These are distinct from the 192 vertex constants and are absent from `RecompD3dPresenterDrawCommand`. Read effective overrides as well as retained shader structure. |
| Texture factor | `draw.texture_factor` preserves packed ARGB, but the host uses it only for selection/modulation. It is a direction candidate only if the original operand routing and signed mapping establish that use; a constant tint is not evidence. |
| Vertex color / texture coordinates | Trace the original operand back to diffuse/specular color or a varying, possibly through a normalization texture. Diffuse bytes are accessible through the FVF layout; the host treats them as color. Specular and arbitrary vector varyings are not fully represented by its current shader interface. Do not treat evaluated lighting RGB as an encoded vector. |
| EMBM | Inspect texture modes, dependent source, bump matrix and luminance scale/offset. These describe UV perturbation and need not encode a recoverable directional sun. |

Public [Cxbx pixel-shader types](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/XbD3D8Types.h)
describe the separate combiner constants and mappings; its
[combiner definitions](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/XbPixelShader.h)
identify constant/color/texture operands, signed input mappings, dot-product
flags and dependent texture modes. Decode the actual mapping, not an assumed
`2*RGB-1`. [NVIDIA's texture-space bump-mapping explanation](https://developer.download.nvidia.com/assets/gamedev/docs/Texture_Space_Bump_Mapping.pdf)
distinguishes normal/light dot products from EMBM's texture lookups;
[Microsoft's EMBM setup](https://learn.microsoft.com/en-us/windows/win32/direct3d9/using-bump-mapping)
specifies its bump matrix and luminance controls. Bump mapping therefore does
not guarantee a world-space sun vector or sun color. For a tangent-space vector,
recover the actual tangent basis, handedness and sign before transforming to
world space; a view-dependent half vector is not the sun. Trace light RGB
separately through the combiner's final modulation.

**Identifying the sand draw:** there is no sand/material-name tag or original
pixel-shader identity in the RTGI command. In a later private frame capture,
correlate draw ordinal, target, projected triangle coverage, world geometry,
FVF/stride, texture bindings and depth/blend state with visible sand. A large
ground mesh with repeating textures is only a candidate; water and shadow
overlays can overlap it. Confirm its screen footprint using draw isolation or
a debug ID overlay during a separate test session, not the ongoing play session.
At RTGI, distinguish the ground receiver from non-depth-writing bump/shine layers
which may not enter the BVH at all.

The [existing capture](../../../recomp-runtime/d3d_draw_adapter.c#L1133) records
pixel-shader binding, stage arguments and, conditionally, stage words and raw
vertex constants even for declined programmable draws. It does not capture a
complete effective pixel-combiner program. The smallest additional diagnostic
would snapshot that program, effective constants/overrides and consumed vertex
attributes **at the adapter before rejection**, paired with a draw ordinal;
keep raw data private. After confirming one material pattern, pass only its
decoded semantic light data to the presenter/RTGI. Do not add raw device decoding
to the render worker or assume an RTGI-only logger can see rejected draws.

**Falsifiable check:** identify the normal-map input and its actual dot-product
partner, or establish that the effect is EMBM instead. Across two camera angles,
the recovered world-space diffuse-light vector should agree for the same scene;
camera-dependent values require checking for eye/half-vector use. Replaying the
captured arithmetic with a flat normal versus the sampled normal must explain
the isolated sand shading. If no such operand/dataflow exists, reject this lead
as a sun source rather than assigning an arbitrary constant slot.

### What RTGI retains and loses

At `d3d_rtgi_add_draw`, the full command and eligible D3D11 texture views are
available. Inside RTGI, only positions, normals, motion, indices, opacity UVs and
opacity material data survive. Neither directional lighting nor vertex/material
RGB is retained. At `d3d_rtgi_apply`, the extra inputs are resolved color, depth
and the target; no light or shadow metadata arrives there.

[Scene admission](../../../recomp-runtime/d3d_rtgi.cpp#L514) keeps only supported,
main-target, depth-writing, non-pretransformed fixed-function triangle draws.
It records entire submitted meshes, so hidden or clipped triangles within those
draws can be hit. Geometry culled before submission, offscreen-target draws,
programmable geometry and non-depth-writing layers are absent. Draws after the
first main-target pretransformed draw are also outside that frame's GI capture.
The pass otherwise runs at present; see [application boundary](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L3539).
This is not a complete world BVH, and a radiance cache would not make it one.

### Smallest sun experiment

First use the existing bounded draw capture in a later, scheduled play session,
keeping raw records private. Compare enabled light direction/color across world
and character draws, camera rotations and a scene transition. Correlate candidate
direction with the game's projected shadow displacement. Add a diagnostic
visibility output only after a consistent candidate is found; an explicit
temporary direction can test the ray implementation but cannot prove extraction.

Retain that candidate and validity with the captured scene, then add one
`RayQuery` toward it from each eligible half-resolution receiver in `traceMain`.
Use the existing BVH, a normal offset, tested near bias, finite distance excluding
the distant sky shell, and `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH`. Reuse the
opacity test before committing candidates. Do not force opaque or cull all
nonopaque candidates: the current BLAS marks its entire geometry nonopaque.
These operations are specified by [Microsoft's inline raytracing contract](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#inline-raytracing).
One fixed direction gives hard shadows; penumbra sampling is later work.
The current 5 cm layer exclusion needs a separate contact-shadow check because
it can skip legitimate nearby casters.

For a first color composite, admit only receivers whose directional contribution
can be reconstructed. Preserve a separate visibility signal; it must not replace
the GI history count or mix direct visibility into the signed bounce estimator.
For a simple unsaturated material, let `B` be its original shading and `D` the
selected direct term in the same color domain. Apply
`B' = B - (1 - visibility) * D`, preserving ambient and other lights. Merely
multiplying the entire framebuffer by visibility also removes ambient, baked
light and reflections. Vertex interpolation, saturation and texture combiners
make that subtraction approximate unless the same raster terms are retained;
exclude those unsupported receivers initially.

**Coexistence:** preserve the game's shadows. Identify their actual draw sequence
with texture, transform, blend and stencil evidence, then record a separate
receiver coverage mask while those draws execute, honoring their depth/stencil
and sampled coverage. Suppress added sun attenuation throughout that footprint,
including its soft edge. Do not infer it from final stencil or classify every
blended draw as a shadow. Until this classification exists, restrict the experiment
to known receivers outside those footprints or show visibility only. Baked
shadows likewise require an excluded/validated receiver set. This deliberately
adds no stronger overlapping shadow; replacing the game's shadow system is a
separate experiment. Compare with GI held constant, since its contact darkening
is another existing contribution.

**Falsifiable checks:** a candidate stays world-fixed during camera rotation and
predicts the observed shadow direction; failure rejects automatic sun selection
for that scene. In a synthetic plane/blocker scene, visibility follows the
blocker, alpha holes transmit, and an unblocked receiver is unchanged. With a
synthetic existing projected shadow, enabling sun shadows changes masked pixels
by at most one 8-bit code value while a separate unmasked receiver darkens.
Sky, UI and target alpha remain unchanged. Implement these later in the existing
[RTGI test harness](../../../recomp-runtime/d3d_rtgi_test.cpp), not in the game runner.

## 2. Off-screen and hidden bounce color

The current [bounce estimator](../../../recomp-runtime/d3d_rtgi.hlsl#L228) samples
screen color only when projection, facing and depth agree. Otherwise a hit
contributes contact darkening within 0.5 world units and zero beyond that.
No unseen color can enter through temporal accumulation alone.

The proposed costs below are arithmetic budgets, not measured timings. They
assume a 3840x2160 target and 1920x1080 tracing: **2,073,600 samples/frame**.
Except where noted, they retain the current primary and bounce rays. All cache
options still need a source of shaded radiance. AMD's
[GI-1.0 paper, sections 2.2–2.3](https://gpuopen.com/download/GPUOpen2022_GI1_0.pdf)
explicitly evaluates lighting at hit points to populate its world cache and
discusses hash quantization/light leaks; caching does not solve missing shading.

| Option | Fit and missing work | Illustrative incremental cost at 4K |
| --- | --- | --- |
| **Hit-point UV + texture RGB** | Reuse committed primitive/barycentrics and existing material capture. Supplies color for never-visible surfaces that are present in the BVH. Needs ordinary opaque UVs, RGB views, material/tint evaluation and supported UV/sampler rules. No persistent lighting history. | No extra rays or denoise passes; at most 2.07M RGB lookups/frame. A separate 256x256x256 RGBA8 array is 64 MiB; copies occur for new/changed slices. |
| **World-space hash radiance cache** | Retains shaded hits through camera motion; a screen-fed version only remembers previously visible surfaces. Needs update/resolve, age/confidence, collision handling, normal/side separation and invalidation for moving meshes and scene changes. [SHaRC integration](https://github.com/NVIDIA-RTX/SHARC/blob/main/docs/Integration.md) uses sparse path updates, resolve and hit queries. | A deliberately small 262,144-entry SHaRC-style 40-byte layout is 10 MiB, before optional locks; its documented 4,194,304-entry baseline is 160 MiB. A proposed 1/16 update rate is 129,600 paths/frame, plus their bounce/light rays, a resolve pass and up to 2.07M lookups. |
| **Irradiance probes / DDGI** | Visibility-aware irradiance interpolation reduces leaks and amortizes diffuse integration. Needs probe placement/scrolling, distance moments, update shading and invalidation. It stores incoming irradiance, not a hit's material color: conversion to outgoing radiance still needs material and direct light. See the [DDGI paper](https://research.nvidia.com/publication/2019-05_dynamic-diffuse-global-illumination-ray-traced-irradiance-fields). | Example: 16x8x16 probes x 128 rays = 262,144 rays/full update; updating 1/8 is 32,768 rays/frame, plus light visibility. Assumed per-probe 8x8 RGBA16F irradiance and 16x16 RG16F distance tiles total 3 MiB, excluding ray data/metadata. Receiver gathering could replace the pixel bounce ray in a larger redesign. |
| **Surfel cache** | Stores lighting on surfaces; useful around detailed geometry. Needs allocation/coverage, spatial lookup, lifetime and attachment to deforming meshes. Current previous-vertex motion is not stable surfel ownership. [EA's GIBS presentation](https://www.ea.com/seed/news/siggraph21-global-illumination-surfels) covers dynamic/skinned geometry and irradiance integration. | Example: 65,536 surfels x assumed 64 bytes = 4 MiB plus lookup/queues. Refresh 1/4 with 16 rays each: 262,144 rays/frame plus hit shading, integration and receiver gathering. |
| **ReSTIR GI** | Reuses path samples across pixels/time; it is not a source of unseen radiance. Requires shaded secondary hits, reservoirs, reprojection, weights/Jacobians, visibility checks and denoising. Much larger change than adding RGB at the existing hit. See the [ReSTIR GI paper](https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing). | Two assumed 32–48-byte reservoirs/sample occupy 126.6–189.8 MiB, before other path data. Candidate generation still shades paths; one or two visibility/reconnection checks per sample add up to 2.07–4.15M ray queries plus resampling passes. This is a proposed budget, not an RTXDI layout or measured cost. |

### First color experiment

Use hit-point texture lookup as the fallback when the existing screen-color test
fails. The required changes are more than a shader sample:

- [RTGI UV capture](../../../recomp-runtime/d3d_rtgi.cpp#L529) currently copies UV0
  only during supported alpha testing. Opaque draws receive zero UVs and a
  sentinel material. Record real color UVs and a material ID for supported opaque
  draws too, including linear-texture coordinate scaling and address modes.
- The [shared cache](../../../recomp-runtime/d3d_rtgi.cpp#L375) is R8 opacity;
  `alphaPS` copies only source alpha. D3D11 RGB is available at the submission
  seam, but not bound as material RGB in DXR. Reuse its shared-resource/copy and
  lifetime pattern for a small bounded RGB array. Keep the separate alpha-mask
  path intact: its texture and UV set can differ from color. Widening R8 to RGBA8
  would cost 48 MiB extra at the current capacity, but is sufficient only when
  the same texture/UV supplies both; a separate array costs 64 MiB.
- Start with static, single-texture fixed-function modulation. Capture vertex
  tint or evaluate the admitted material/directional shading, then interpolate
  at the hit. Exclude reflection, four-tap, separate-mask UV, render-target and
  programmable combinations until explicitly supported. Use explicit texture
  LOD because a ray hit has no raster derivatives. DXR supplies the primitive
  index and barycentrics needed for interpolation; see the
  [RayQuery reference](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#rayquery).
  Also reject unclassified original bump/pixel-shader materials at the adapter:
  `program_count == 0` alone is insufficient, and stage 0 could hold normal data
  rather than surface color.
- Treat the sampled result as an approximate outgoing-color proxy. Raw albedo
  alone is not radiance and must not make a dark surface glow. Preserve baked
  shading/tint where known; physically shading it needs incident light and
  visibility. Match the current UNORM signal for this bounded comparison; do
  not silently mix linear radiance with its display-domain signed estimator.
  Retain the existing screen path and unsupported-material contact fallback.

This handles hidden color without cache warm-up. It still cannot illuminate
from a mesh absent from the recorded BVH. If camera rotation reveals missing
submitted geometry rather than missing color, stop and measure that coverage
problem before adding any cache. A later screen-fed hash can preserve previously
seen shading, but cannot supply never-seen materials or missing occluders.

**Falsifiable check:** extend the synthetic red-wall/floor fixture with a textured
wall outside the camera view but inside RTGI capture, farther than contact range.
Keep geometry and rays fixed; swapping red for blue must swap the dominant bounce
channel on the visible neutral floor after 32 frames. Removing that wall must
remove the color contribution. Rotate across its visibility boundary and check
for no abrupt hue change between screen and texture paths. Unlit-material,
alpha-hole and unsupported-material controls must retain their intended fallback.
Measure median whole-apply GPU time at 4K with fixed geometry/MSAA and warm-up;
use an initial acceptance budget of **at most 10% extra time**, explicitly a
target to test, not a prediction.

## Current GPU work inferred from code

At 4K, [dispatch sizing](../../../recomp-runtime/d3d_rtgi.cpp#L843) is
`240 x 135` groups of `8 x 8` threads for each half-resolution compute pass.

| Work | Upper workload per frame |
| --- | --- |
| Geometry | CPU skinning/copy; one merged BLAS and one single-instance TLAS rebuilt with `PREFER_FAST_BUILD`, not refit. Upload payload is about 44 bytes/vertex + 16 bytes/recorded triangle + 16 bytes/opacity material. Double-sided geometry duplicates triangle indices. |
| Input | Full-resolution color copy/resolve, half-resolution color/depth reduction; dirty opacity slices each require a 256x256 raster copy. |
| Trace | **Two queries**, not one: `primarySurface` recovers triangle normal/motion for non-sky depth, then a cosine bounce ray for eligible receivers. Up to **4,147,200 queries/frame**, or 248.8M/s at 60 fps; sky/far/invalid pixels reduce this. Alpha candidates add shader work. |
| Temporal | One dispatch, 25-neighbor statistics: up to 51.84M neighborhood visits. History weight caps at 32 samples; it does not launch 32 rays/frame. |
| Spatial | Four a-trous dispatches, steps 1/2/4/8, nine taps each: up to 74.65M neighborhood visits. Step 8 bypasses filtering where history count is at least eight. |
| Composite | Full-resolution, depth-aware four-tap GI upsampling and target blending; MSAA executes per sample. At 1x, up to 8.29M pixels and 33.18M upsample taps. Brightening is additionally limited by color headroom. |
| Synchronization | D3D11 signals input readiness; D3D12 waits, builds/traces/filters and signals; D3D11 waits before compositing. Three allocator/resource slots do not remove this same-frame dependency. |

Sources: [BVH build](../../../recomp-runtime/d3d_rtgi.cpp#L691),
[trace and filters](../../../recomp-runtime/d3d_rtgi.hlsl#L134),
[composite](../../../recomp-runtime/d3d_rtgi.cpp#L221), and
[cross-API scheduling](../../../recomp-runtime/d3d_rtgi.cpp#L888).
The six compute dispatches total 12.44M thread invocations/frame. Half-resolution
images occupy about **205.7 MiB**, plus **16 MiB** opacity storage, excluding BVHs,
geometry slots, allocation padding and existing D3D11/MSAA targets. Shared views
alias storage; they do not imply a second complete image allocation.

One sun query adds at most 2.07M rays/frame (124.4M/s at 60 fps), bringing the
upper query count to 6.22M/frame. A separate half-resolution R16F visibility image
would be about 4 MiB before history/filtering. Cost is not proportional to ray
count alone: early termination, alpha tests, BVH quality, bandwidth and cross-API
waits matter. The existing [timing test](../../../recomp-runtime/d3d_rtgi_test.cpp#L809)
sets a 3 ms threshold for a synthetic 1920x1080 target; that is neither an observed
result here nor a 4K estimate. 4K has four times its pixel work, with geometry
costs scaling separately. GPU timestamps on the target adapter are required for
a millisecond claim.

## Draw census (2026-09-30)

Bounded `RECOMP_D3D_DRAW_CAPTURE` runs on `codex/rtgi-dxr` `de393c9`, three
attract-mode frames: title flyover (1,196 draws), a sunset beach close-up
(238) and a daytime jungle close-up (239). Raw records stay under `private/`.

- **No pixel shaders or programmable vertex shaders.** Every draw in all three
  frames used a fixed-function FVF with no pixel shader handle, and every draw
  was accepted. Texture stages used only MODULATE, SELECTARG1, ADD,
  BLENDTEXTUREALPHA and MULTIPLYADD (Xbox D3DTOP numbering from
  [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/XbD3D8Types.h)).
  No DOTPRODUCT3, BUMPENVMAP or BUMPENVMAPLUMINANCE stage appeared. The
  bump-mapping lead is unconfirmed for these scenes; the combiner support gap
  above does not affect them.
- **Point lights are the lighting gap that matters.** In the sunset frame, 96
  character draws (about 40% of the frame's draws) enable lighting with two
  zero-color directional lights plus a warm key and a dim fill *point* light
  with distance attenuation. `attach_directional_lighting` rejects any
  non-directional light, so these draws render without their scene lighting.
  In the jungle frame, characters use four directional lights and are lit.
- **Other unlit draws are correct.** The remaining lit-but-unattached draws
  select texture color only (SELECTARG1), so vertex lighting does not change
  their color on the original path either.
- **Sun candidate.** Environment draws in the sunset frame carry three
  directional lights; one is several times brighter and warm, at a low
  elevation consistent with the visible sunset. It is a candidate only; the
  world-fixed check under camera rotation is still required.

Limit: the capture covers the intercepted draw entry points; a scene drawn
through another path would not appear.

**Next step:** add D3D point lights to the host lighting path, per the
[D3D8/9 fixed-function light model](https://learn.microsoft.com/en-us/windows/win32/direct3d9/mathematics-of-lighting)
(attenuation `1 / (a0 + a1·d + a2·d²)` within range), which needs world-space
vertex positions alongside the existing normal transforms. Check: in the sunset
frame, those 96 draws attach lighting and the character shows a key-lit side.
