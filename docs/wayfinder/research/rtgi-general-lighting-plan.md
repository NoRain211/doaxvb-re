# RTGI general lighting plan

2026-09-30. Planning only; no builds, runners, captures or GPU measurements were
performed for this document. Source baseline: `codex/rtgi-dxr` at `ee2fbae`.
Implement in order: game-light shadows, game-light bounce, then baked-light
replacement. Every enabled directional and point light applicable to a surface
must participate; spot lights remain unsupported. Preserve the D3D11 fallback.

Each stage has its own branch and separate worktree, stacked in this approved
order. Keep stage worktrees under `private/` or the Codex worktree directory.

| Stage | Branch | Base |
| --- | --- | --- |
| 1: shadows | `codex/rtgi-shadows` | `codex/rtgi-dxr` |
| 2: bounce | `codex/rtgi-bounce` | `codex/rtgi-shadows` |
| 3: relight | `codex/rtgi-relight` | `codex/rtgi-bounce` |

Every stage is opt-in at runtime with its own toggle. Disabling a later stage
retains enabled earlier stages; disabling a prerequisite makes its dependents
inactive without silently enabling it. With all stages off, preserve the base
renderer and original shadow draws. These are implementation instructions, not
branches or worktrees created by this planning change.

The [background review and draw census](rtgi-sun-and-radiance-cache.md) found
character key/fill point lights and environment sets of three or four directional
lights. Its bright sunset sun is only a candidate, not a reason to discard other
lights. Point-light support from `codex/point-lights` now reaches D3D11 through
[`attach_directional_lighting`](../../../recomp-runtime/d3d_draw_adapter.c) and
the [presenter command](../../../recomp-runtime/d3d_presenter.h). The current
[RTGI shader](../../../recomp-runtime/d3d_rtgi.hlsl) also has an off-screen
texture/tint lookup, but that color has no hit-point light evaluation. It is not
shaded radiance. This plan supersedes the background's preserve-and-mask policy
for projected shadows: suppress confirmed projected/blob shadow draws when their
RTGI replacement is active.

**Shared acceptance and budget.** Hold 60 fps at 3840x2160 with 8x MSAA on the
user's target GPU; record GPU/driver, build SHA, settings and the resolved
`origin/main` SHA in private evidence. Use 1920x1080 tracing (N = 2,073,600
receivers), independent of MSAA sample count. Estimates below are query counts,
not timing predictions. Measure whole-frame CPU/GPU time, BVH construction,
trace/filter/composite and D3D11/D3D12 waits. Require warmed gameplay windows
with p99 frame time <= 16.67 ms and no sustained missed presents; measure with
frame dumping disabled. If the baseline already exceeds the budget, report the
shortfall rather than claiming the target from isolated shader timing.

For every stage use `private/shot.ps1` for paired dumps of the same scene frame:
stage on, stage off with prior stages unchanged, and `origin/main`. Fix camera,
pose, settings, random seed and temporal warm-up; matching present indices alone
does not prove matching content. Confirm actual target dimensions. Cover beach,
sunset Hitomi (~3000), jungle (~5400), Christie (6300-6600), and title flyover
(360-600); choose and retain a reproducible beach frame. These are scene locators,
not a requirement to time a long demo as one bounded iteration. Keep captures
and detailed run evidence private. A named-build user play test accepts visual
faithfulness; synthetic checks and frame presentation alone do not.

## Stage 1: shadows from all game lights

**Approach.** Capture the frame's light list with the RTGI scene and retain
per-draw light-set membership on receivers/triangles. Never use the last draw's
lights globally or apply character fill lights to unrelated environment draws.
Start with one or two shadow queries per eligible half-resolution pixel, sampling
its light set by brightness, point attenuation and positive normal cosine.
Give every nonzero contributor nonzero probability. For K samples, estimate the
occluded direct term as `sum((1 - V_i) * D_i / p_i) / K`; temporal accumulation
fills in unsampled lights, with rejection on disocclusion, light-set changes and
scene cuts. This is ordinary importance sampling; [PBRT's direct-light
estimator](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer)
includes light-selection probability in the weight. [ReSTIR DI](https://research.nvidia.com/publication/2020-07_spatiotemporal-reservoir-resampling-real-time-ray-tracing-dynamic-direct)
is a later option if simple sampling fails the noise/cost gate, not a required
reservoir subsystem for these small sets.

Use alpha-aware inline `RayQuery` visibility with first accepted hit termination,
tested origin bias, a point-light endpoint, and a scene-bounded directional ray
that excludes the sky shell. Do not force nonopaque geometry opaque; see the
[DXR inline raytracing specification](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#inline-raytracing).
Identify projected/blob shadow sequences using draw isolation plus stencil,
blend, depth and coverage evidence. Suppress their color contribution only while
RTGI shadows are effective, preserving stencil side effects needed by later
draws. Restore them on disable/fallback. Do not classify every blended draw as
a shadow or infer shadow coverage from the final, possibly cleared stencil.

**Data needed.** Copy light type, RGB, world-space direction/position, range and
attenuation into scene-owned storage with draw-to-set IDs and change detection.
The adapter's directional vector already points toward the light. Match the
game's [point attenuation](https://learn.microsoft.com/en-us/windows/win32/direct3d9/attenuation-and-spotlight-factor),
`1 / (a0 + a1*d + a2*d*d)` within range, rather than assuming inverse square.
Current material-path admission and its eight-light cap do not prove complete
capture: audit all lit draws, capture semantic light state independently of
shading admission where needed, and report unsupported/overflow cases. Keep
guest-state decoding at the adapter. Retain the original direct contribution
separately from ambient/emissive, baked color and reflections so visibility
replaces only direct lighting. Initially admit reconstructible material paths;
missing applicable light sets in the acceptance scenes remain unfinished work.

**Cost estimate.** Add K*N = 2.07-4.15M shadow queries/frame, on top of the
current <=2*N primary/bounce queries: <=6.22-8.29M total. Two RGBA16F direct-term
history images would add 31.6 MiB plus light/set metadata. Reuse existing
reprojection/filter infrastructure; account for extra filtering and 8x composite
work in the whole-frame gate.

**Risks.** Saturation and vertex interpolation can invalidate naive subtraction;
retain raster terms or restrict admission instead of multiplying the whole image
by visibility. Baked shadows cannot be removed yet. False shadow classification,
missing BVH casters, alpha holes, stale history and overly large bias can hide
errors or double-darken. Keep visibility history separate from signed GI.

**Falsifiable acceptance.** In the existing synthetic RTGI harness, compare two
colored directional lights plus attenuated points against an all-lights shadow
reference: after 256 frames, mean absolute RGB error on unsaturated receivers
must be <=2% of the reference peak. Occluding either light must remove only its
term; point range, alpha holes and a dim fill must work. Different draw light
sets must not leak into each other. An unblocked receiver must remain within
one 8-bit code value. A projected/blob shadow fixture must lose its legacy color
draw only when replacement is active, without breaking later stencil consumers.
Pass the shared scene matrix with no duplicate shadow, sky/UI/alpha regression
or missing supported lights, plus the 60 fps gate.

## Stage 2: bounce from game lights, including off-screen hits

**Dependency:** Stage 1 accepted; reuse its scene light sets and visibility.

**Approach.** At the existing cosine-weighted bounce hit, evaluate direct incoming
light with that hit triangle's light set, importance weight and shadow visibility;
apply the hit's diffuse material response to obtain outgoing radiance. Feed that
into one-bounce transport with the receiver's response and sampling PDF. Use the
same evaluator for visible and off-screen hits so crossing the camera boundary
does not change lighting. Texture/tint remains an approximate reflectance input
until Stage 3; it must never emit merely because it is bright. Replace the old
screen-color bounce contribution for supported materials rather than adding a
second copy. Define linear-light decoding, transport and final encoding explicitly;
do not mix physical radiance directly with the existing UNORM signed-sky delta.
Keep ambient/emissive accounting separate from the game-light bounce term.

**Data needed.** Reuse captured RGB/UV/tint material data; retain smooth/geometric
normals, material identity, explicit texture LOD and triangle light-set IDs at
secondary hits. Audit sampler, alpha-mask and material coverage. Off-screen
surfaces must exist in the captured BVH: a cache cannot invent unseen geometry.
Use direct hit shading first; add a radiance cache only if measured shading cost
or missing history warrants it. [PBRT's path-tracing estimator](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer)
provides the transport/PDF basis; the [background review](rtgi-sun-and-radiance-cache.md)
compares optional cache designs and their coverage limits.

**Cost estimate.** Retain one bounce ray and add one sampled light-visibility ray
per valid hit: <=2.07M extra queries/frame, <=8.29-10.37M total with Stage 1.
Up to N material/light evaluations replace the texture-only fallback. Reuse the
existing color cache and GI history; no persistent radiance cache allocation is
required. Include texture updates and secondary alpha traversal in timing.

**Risks.** Baked shading in texture/tint biases bounce energy until Stage 3;
double-applied vertex lighting, gamma errors and screen/off-screen mismatches
can create glow. Missing off-screen geometry is a capture failure, not a denoiser
failure. Light changes must invalidate affected history.

**Falsifiable acceptance.** Keep a red wall outside the camera but in the BVH,
beyond contact range, under a directional or point light. After 32 frames the
neutral floor's added bounce must be red-dominant; swapping to blue must swap the
dominant channel. With other sources disabled, switching off or fully blocking
the light must remove at least 95% of that contribution; deleting the wall must
remove it too. Camera rotation across the wall's visibility boundary must change
the steady receiver contribution by <=5% under fixed geometry/light. Repeat with
different hit/receiver light sets, then pass the shared scene and performance
gates without duplicating ambient or the old screen-space bounce.

## Stage 3: estimate albedo, remove baked light, and relight

**Dependency:** Stage 2 accepted. Do this last; its branch/worktree and opt-in
runtime toggle isolate the code changes. Albedo estimation and relighting are
committed scope, not an optional follow-up.

**Approach.** For validated diffuse surfaces, model the baked source color in
linear space as `C_baked ~= albedo * L_baked`. Estimate the original baked light
per surface from known material/vertex terms, normals and the game's reference
light sets, then compute `albedo = clamp(C_baked / max(L_baked, epsilon), 0, 1)`.
Separate tint, emission and specular terms before division. Keep the reference
estimate stable per material/surface; dividing by current changing lights would
cancel the intended relighting. Replace the baked diffuse contribution with
Stage 1 direct light plus Stage 2 indirect light using this albedo at both visible
and secondary hits. Do not add new illumination on top of the old bake.

**Data needed.** Original linear diffuse color, material/source provenance,
reference lighting and surface normals, confidence and a bounded denominator.
Retain source color for fallback and invalidate estimates on material changes.
Expose estimate/albedo/relit debug views for private comparison. Start with known
diffuse materials; retain unsupported or low-confidence surfaces explicitly.

**Cost estimate.** No additional rays. One bounded RGB division and relight per
eligible sample, with the same material work at bounce hits. If stored, one
RGBA16F estimate/confidence image at tracing resolution costs 15.8 MiB; prefer
existing material storage where the estimate is constant. Measure bandwidth and
invalidation cost within the unchanged whole-frame budget.

**Visual accuracy risks only; code risk is isolated.** Color alone cannot
uniquely separate reflectance and illumination;
this is an approximation, not recovered original albedo. Dark denominators,
clipped highlights, painted details and baked shadows can become bright stains
or lose artistic intent. Never treat texture contrast alone as lighting. Freeze
and validate the estimator before broadening material coverage; report residual
bakes rather than claiming full replacement. These are visual acceptance checks
to resolve during Stage 3, not reasons to avoid implementing it.

**Falsifiable acceptance.** A synthetic textured diffuse surface with known
albedo and baked illumination must recover albedo within 5% mean absolute error
on valid, unsaturated samples. Restoring its reference illumination must reproduce
the source within two 8-bit code values; moving/changing only the light must
change shading while recovered albedo stays fixed within 1%. Zero-light and
low-confidence cases must stay finite and retain fallback without white spikes.
In all shared demo scenes, paired dumps must show removed baked diffuse shading
on admitted surfaces without erased texture detail, doubled lighting, new seams
or flicker, accepted by the user, while preserving the 60 fps target.
