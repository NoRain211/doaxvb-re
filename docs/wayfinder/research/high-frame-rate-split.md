# Split-rate animation and camera rendering

Design: 2026-10-03. M4 checkpoint: 2026-10-05. Opt-in numeric split-rate
presentation has passed the bounded replay, pose, pacing and smoke gates below.
The local build is ready for a user play test; broader release coverage remains open.

## Recommendation and evidence limits

Keep gameplay, root motion, physics, AI, input consumption, animation events and
stateful effects at 60 ticks/s. Evaluate a separate visual pose and camera at
each requested present time and rasterize through D3D11. The game already has fractional
animation sampling, but its entry point also changes root-motion state. Calling
the existing update twice is not a safe implementation.

Prefer replay of a tagged, resource-safe draw packet over a second invocation of
the character draw/update traversal. At the initial checkpoint, the packet lacked bone/camera provenance and retained
some borrowed resources. The milestones below add that provenance and ownership
for bounded replay.

The initial investigation used bounded read-only Ghidra queries, the configured
local generated program, the tracked runtime, and a separate private
model/animation survey. It preceded the runtime observations recorded below.
Ghidra image
identity: SHA-256
`053d44e885fa33c1d15d909a533f39dfbd976e97eeaf67e4fdef8438ea7e5c54`,
image base `0x00010000`. Addresses below refer to that image. Generated entry
names use `sub_` followed by the uppercase eight-digit address. Ghidra's
`FUN_` functions sometimes span several generated entries; replacing only the
first fragment without checking continuations is insufficient.

## Animation storage and evaluation

The private archive survey identifies banks with 1,000 clip slots, aliases and
bank-relative offsets. A clip has two 16-bit header fields followed by 60
clip-relative track offsets; zero offsets denote absent tracks. **Sixty is a
track count, not a keyframe rate.** Tracks are independent scalar streams, not
arrays of complete bone matrices or float quaternion poses. The survey's
duration sums agree with the clip endpoint count for most tracks, with exceptions
that still need decoder-level validation.

The executable provides a stronger interpretation of those streams:

| Guest address / generated entry | Observed responsibility |
| --- | --- |
| `0x000AEC80` / `sub_000AEC80` | Selects a clip, initializes 60 track cursors through channel-remapping tables, substitutes a default for absent tracks, and invalidates pose caches. |
| `0x000AE850` / `sub_000AE850` | Walks variable-duration segments using the low two header bits as type and the next ten as duration; maintains segment start/end and coefficient state. This cursor is mutable. |
| `0x000AE980`, `0x000AE9F0` | Decode compact value and tangent representations into floating-point values. These are custom encodings, not ordinary IEEE half floats. |
| `0x000AEA60` / `sub_000AEA60` | Builds cubic Hermite coefficients from endpoint values, endpoint derivatives and segment duration; the record step is ten bytes. |
| `0x000AEB50` / `sub_000AEB50` | Builds a linear segment, zeroing quadratic/cubic coefficients; the record step is four bytes. A terminal segment supplies a constant value. |
| `0x000AF050` / `sub_000AF050` | Evaluates 60 scalar channels at an integer clip index with Horner evaluation of the segment polynomial; uses the pose-cache selector `0x000AEFF0` and an optional mirrored-channel transform `0x000AFD20`. |
| `0x000AEEF0` / `sub_000AEEF0` | Splits a floating cursor into integer and fractional parts, samples adjacent integer poses, and blends them. Fractions at or below approximately 0.01 use the first sample. Also accumulates root displacement: it is not a pure sampler. |
| `0x000AF5C0`, `0x000AF6A0` | Dispatch masked/channel-specific pose blending. Translation channels use linear blending; rotation/vector channels use several different helpers. |
| `0x000AC550`, `0x000AC690` | Save the previous pose for transitions, advance transition age, combine two animation layers and blend against the outgoing pose. |
| `0x000ADFB0` / `sub_000ADFB0` | Advances a floating clip cursor and integer/event cursors, and updates completion/loop-related flags. |
| `0x000AC100`, `0x000B2230` | Coordinate live animation layers and process timed commands. Keep their event effects on the simulation tick. |

For a Hermite segment of duration `d`, endpoints `p0,p1` and derivatives `m0,m1`,
the recovered coefficient construction is consistent with
`c0=p0`, `c1=m0`, `c2=(3*(p1-p0)/d-2*m0-m1)/d`, and
`c3=(-2*(p1-p0)/d+m0+m1)/(d*d)`. The sampled value is
`((c3*x+c2)*x+c1)*x+c0`, where `x` is the integer sample index minus segment start.
This is a description of the algorithm, not copied generated source.

For compatibility, preserve the existing sequence **integer curve samples,
then channel-aware fractional blend**. Directly evaluating every cubic at a
fractional index is an attractive later change, but it need not reproduce the
existing rotation, mask or transition behavior.

Rotation inputs include Euler-angle triples. `0x0017C570` constructs endpoint
rotation matrices and interpolates a relative axis/angle; `0x0017C250` extracts
Euler angles again. Other channel kinds blend directions or wrapped angles.
It would be incorrect to describe the entire player as quaternion SLERP or as
linear Euler interpolation. Exact singularity and wrap behavior needs tests
before replacing the math helpers.

The live cursor increment at `0x000ADFB0` multiplies layer speed, two actor speed
factors and the global speed at `0x004D6FC4`, then adds a small epsilon of roughly
`1e-6`. Initialization at `0x000A4680` sets global speed to 1; `0x000A53C0` changes
it for speed modes. This is a clip-index clock advanced by calls, not an elapsed
seconds API. With all speed factors equal to 1 and one call per 60 Hz tick, the
nominal scale is one clip index per tick, or 60 indices/s. Variable-duration keys
do not have a uniform keyframe frequency, and this static result does not prove
every route runs its update once per tick. Slow motion, pauses and route-specific
speeds must be captured rather than replaced with a universal `time * 60`.

## Skeleton, procedural pose and delivery to D3D

The character context initialized at `0x00062630` points to per-character matrix
banks at `0x004D3650 + actor*0x800` and `0x004D72F0 + actor*0x800`: 32 slots of
64 bytes each. These are runtime matrix banks, not proof of 32 directly keyed
bones. Sixty scalar channels include translations, rotations and limb targets;
the final pose also depends on procedural assembly.

The matrix-producing cluster is:

- `0x000B01E0`: assemble the character pose from sampled channels and actor state.
- `0x000B02E0`, `0x000B0370`: root/body matrix composition and terrain-related
  correction. The latter calls the ground-height helper and adjusts the body.
- `0x000B1B50`: target-directed angular correction with clamps and a persistent
  blend weight, followed by matrix writes. This is a look-at-style path; exact
  anatomical naming still requires a controlled visual check.
- `0x000B0B20`, `0x000B0C50`, `0x000B1290`, `0x000B16A0`: four limb passes, target
  construction, two-link length/angle solving and final orientation. The distance
  and cosine-law calculations establish inverse-kinematics behavior.
- `0x000B20A0`, `0x000B3190`: additional joint-channel evaluation. The latter has
  its own cursor and commands; do not treat this whole path as stateless math.
- `0x000AE350`, `0x000AE3F0`: preserve position history and derive the second
  matrix bank. Their writes must remain distinct from visual scratch output.
- `0x000AE240`: consume root displacement into gameplay position. Never run it
  for a presentation-only sample.

There is separate stateful secondary motion. `0x00056E90` dispatches several
groups; `0x00057300` advances chained nodes using prior displacement, damping,
external forcing, length normalization and constraint/collision helpers.
`0x00057250` dispatches their geometry path. Some integration is guarded by the
game's pause/update flag. This is enough to reject running the whole cluster at
120 Hz, but not enough to assign every group to hair, cloth or breast motion.
Those anatomical assignments, all secondary update laws, and the coverage of
CPU-deformed geometry remain open. Treat hair/cloth/breast motion as 60 Hz
secondary state until each owner is identified; do not claim it is all baked
into the primary clips.

`0x000633E0` interprets character draw records. It chooses matrices and calls
draw callbacks; some opcodes also assign or increment material values.
`0x000636D0` builds the small draw palette at `0x00B25840` from selected pose
matrices and model-offset operations. A draw can therefore require a derived
palette transform, not merely a copied global bone matrix.

`0x0017D670` and `0x0017D9A0` load that palette into WORLD0..WORLD3, enable vertex
blending and submit model subsets. Their rigid path binds one world matrix.
The surveyed weighted vertex layouts have position, zero to three explicit float
weights, normal and UV, with an implicit remaining weight and no interleaved
joint indices. Thus this is **indexed geometry with a per-draw world palette**,
not evidence of arbitrary per-vertex indexed bone lookup. Main weighted meshes
reach host GPU skinning; this does not exclude separate CPU-deformed effects.

The current [draw adapter](../../../recomp-runtime/d3d_draw_adapter.c), especially
`compose_world_view_projection`, `compose_blend_transforms` and directional/
reflection attachment, reads the device's separate matrices and composes them.
The [presenter command](../../../recomp-runtime/d3d_presenter.h) carries one WVP
plus three blend WVPs, normal transforms and optional program constants. The
[D3D11 backend](../../../recomp-runtime/d3d_presenter_d3d11.cpp) uses those for
weighted vertex transformation. Xbox transform slots 0/1 and 6..9 agree with the
public [Cxbx transform definitions](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/XbD3D8Types.h).

## Camera ownership and frame dependence

Camera slots start at `0x0041A800`, stride `0xAE0`, with five possible slots.
The relevant fields recovered from their producers/consumers are:

| Offset | Role |
| --- | --- |
| `+0x50`, `+0x90`, `+0xD0` | View, projection and composed matrix outputs |
| `+0x110`, `+0x148` | Working/target camera parameter blocks used by smoothing |
| `+0x180`, `+0x190` | Final eye and target positions |
| `+0x1B0`, `+0x1B4` | FOV input and roll input |
| `+0x2AC`, `+0x2B0` | Near/far projection inputs |

`0x0002EF20` is the camera orchestration entry. It updates active slots, including
smoothing and effects, then calls `0x00023B10` to build matrices.
`0x0002DFE0` converts between eye/target and orbit angle/distance forms.
`0x00023BB0` builds a look-at view with roll and a near-vertical special case;
`0x00023E80` builds the perspective projection with FOV, aspect/scales, depth
range and offsets. `0x0002DDA0` publishes the chosen camera into render globals,
and `0x0002E8C0` binds world/view/projection through SetTransform.
`0x001221E0` also temporarily binds alternate camera/target state, so one global
camera override would incorrectly affect offscreen passes.

`0x000301C0` smooths selected position/orbit/FOV-related parameters using
`0x00022A80` and `0x00022B40`, mode switches and angle wrapping. Its update is
per invocation. `0x00030800` advances a shake/effect sample pointer, increments
effect ages, updates phases and can consume random numbers. `0x00030AC0` and
`0x00030D20` participate in the final camera adjustment chain. This is not just a
view-matrix copy, and halving one camera delta would not preserve its behavior.

First keep stateful smoothing/shake at 60 Hz and evaluate intermediate visual
eye/target/FOV/roll from captured endpoints, rebuilding the matrices at 120 Hz.
That gives smooth visual camera motion without advancing effect state twice.
A later continuous evaluator can reproduce each smoothing/effect law at fractional
time from copied state. Preserve cuts and mode changes as discrete boundaries;
do not blend across them. A generic exponential coefficient adjustment is not a
substitute for recovering the actual mode-dependent smoothing law.

## Hand-source boundary and time contract

Create independent models for `sample_clip`, `blend_pose`, `build_skeleton`,
`build_draw_palette` and `evaluate_camera`. Inputs are immutable clip/model views
and a visual snapshot; outputs are owned pose/matrix buffers. Register the
game-facing replacements through
[`recomp_lookup_manual()`](../../../recomp-runtime/program_manual.c), retaining
existing guest layouts until every owner is replaced. Keep adapters thin.

The first coherent animation replacement includes the `0x000AE850` decoder
family, `0x000AF050` sampling/cache behavior, pure portions of `0x000AEEF0`,
`0x000AF5C0` blending and required channel helpers. Then move matrix assembly
from the `0x000B01E0` cluster and palette derivation at `0x000636D0` into models.
Freeze tick-owned look-at/IK targets and weights, while recomputing the geometric
solve for each sampled visual pose. Secondary integrators, event scripts, root
motion and transition completion remain authoritative at 60 Hz.

The camera replacement starts with the pure matrix-building portions of
`0x00023B10`/`0x00023BB0`/`0x00023E80` and explicit per-pass camera binding.
Port stateful camera controllers only when a fractional evaluator needs them.
Standard math and D3D/XDK code should receive independent native replacements,
not decompilation of library implementations. Confirm ownership of shared
helpers before porting them. Any temporary generated-library call needs the
documented adapter/lookup seam required by the project rules.

Use an explicit delayed interpolation contract first. After simulation tick `n`
finishes, retain snapshots `n-1` and `n`; render time is
`T=(n-1+t)/60`, with `t=0` and `t=0.5` for 120 Hz. This deliberately adds one
60 Hz tick, about 16.7 ms, of visual latency. It supplies known endpoints for
the ball, contacts, transitions and secondary motion, and keeps all visual
objects on the same timeline. It does not improve the 60 Hz input/AI response.

For an unchanged clip/layer, use its recorded cursor and speed to evaluate the
intermediate clip time. Preserve the sampler's integer-plus-fraction behavior;
evaluate masks and transition weights from their tick snapshots. Do not linearly
blend wrapped cursor numbers across a loop: record the loop/clip transition and
its event boundary. Do not apply visual root displacement back to gameplay.
Discontinuous events, teleports and camera cuts need explicit policies and new
instance generations. Separate visual caches from the mutable guest track caches,
which are designed for advancing sample indices.

An optional lower-latency mode could sample the current clip forward by half a
tick without committing events. It would predict gameplay-driven transitions,
contacts and camera targets and could disagree with the next tick. It should be
described and tested as prediction, not as exact interpolation of known states.

## Extra-frame rendering

The [capture packet](../../../recomp-runtime/d3d_presenter_capture.cpp) copies
vertices, indices, palettes and linear texture data, but borrows some other
texture payloads. The [render worker](../../../recomp-runtime/d3d_presenter_thread.cpp)
executes each packet once and clears it. Neither file currently offers a
persistent, editable scene or a second-presentation schedule.

Extend the packet at the game-to-adapter seam with actor generation, subset and
palette-operation identity, per-pass camera identity, and separate world/view/
projection data. Store the inputs needed to rebuild derived palette transforms;
draw ordinal or a reused vertex-buffer address is not a stable object identity.
Keep a sealed packet and visual snapshots alive until both presentations finish.
Pin or copy resource generations for that lifetime, including resources borrowed
by the current capture path. The worker must not call generated game code or
read mutable guest pose state.

For each presentation, derive an owned command view with the evaluated world
palette and camera, then recompute WVP, normal transforms, reflection world-view
and any identified matrix-bearing shader constants. Leave screen-space UI
transforms alone. Re-issue geometry into fresh/cleared render targets in original
pass order, including shadow/reflection/offscreen dependencies that need the new
pose. Do not replay release/report records or advance guest swap counters,
vblank callbacks, gamma events or simulation bookkeeping twice. Clear/rebuild
only targets that are transient for that visual frame; preserve genuine history
resources according to their owning pass. Backbuffer sampling and feedback
passes require explicit treatment, not blind packet duplication.

For a first admitted scene, captured geometry and draw membership may be held
constant. A shipping version must account for camera-dependent culling, transparent
sorting, LOD, billboards, CPU-generated vertices and newly visible objects.
Matrix substitution cannot recover geometry the game did not submit. Use
conservative visibility for the small interval, or rerun a hand-written pure
visibility/submission stage. In particular, do not rerun `0x00063000` as the
extra-frame renderer: it invokes secondary updates and the mutating draw script.

Host presentation must have its own 120 Hz deadline while guest waits remain at
60 Hz. An extra host present must not unblock an extra simulation tick. Bound
the render queue so two presentations do not grow latency indefinitely; if work
misses 8.33 ms, drop the optional intermediate presentation without changing game
speed. D3D11 remains the default/fallback backend. Performance is unmeasured.

## Objects outside the primary skeleton

| System | First split-rate policy |
| --- | --- |
| Ball and other dynamic rigid objects | Interpolate known position/orientation endpoints on the delayed timeline; do not extrapolate through an unknown collision. Use event-aware piecewise trajectories when contact timing is available. Otherwise hold/snap at contact boundaries rather than invent a path through a surface. |
| Character root | Interpolate authoritative actor placement at the same visual time; sample local pose separately. Ensure baked root displacement is not applied a second time. |
| Static objects | Keep world transforms; reproject through the evaluated pass camera. |
| Particles | Keep spawn/death/RNG/collision at 60 Hz. Match particles by identity, interpolate surviving state, and rebuild camera-facing geometry where needed. Captured CPU vertices otherwise remain at 60 Hz. |
| UI and video | Keep tick-driven logic and decoded video cadence. Redraw/composite at 120 Hz; resample only explicitly time-based UI motion. Do not pretend repeated video frames contain new motion. |
| Hair/cloth/breast and other secondary motion | Keep integration at 60 Hz. Initially hold the secondary result; later interpolate captured local offsets/constraints and reattach them to the new primary pose. State all remaining 60 Hz motion in acceptance notes. |
| Shadows/reflections | Reuse geometry/material resources but rerender dependent passes with the same visual pose; a 120 Hz character with a 60 Hz shadow is an incomplete result. |

## Presenter-only fallback and comparison with whole-game 120 Hz

A smaller fallback retains two matching draw sets and interpolates decomposed
world transforms/palettes plus camera state, then rasterizes geometry again.
It also needs one tick of history, stable draw identity and resource lifetimes.
It cannot recover source clip curvature or rerun IK, and global matrix/palette
interpolation can distort a hierarchy. Never linearly interpolate a composed
WVP matrix: camera projection and rigid rotation need separate treatment.
Use rotation interpolation and affine decomposition only on transforms whose
semantics have been identified; snap unsupported or discontinuous cases.

Both approaches generate new geometry renders rather than interpolating pixels.
The full split design samples the actual animation player and reconstructs the
pose; the fallback approximates motion from two submitted poses. Both retain
60 Hz gameplay under the proposed delayed contract. Running the entire game
at 120 Hz instead requires auditing every per-call timer, event, RNG consumer,
integrator and input edge. The cursor, transition, look-at, limb and camera
state changes found here are concrete examples of why changing presentation
pacing alone cannot establish correct whole-game 120 Hz behavior.

## Smallest experiment and effort

Start with one naturally active character, one unchanged clip and a fixed camera.
Implement the pure decoder/fractional sampler and the required pose/palette path
in hand source. Capture immutable inputs at one admitted tick; evaluate at its
normal cursor and at `cursor + 0.5 * recorded_tick_advance`. Keep visual outputs
outside guest state. For this isolated experiment, hold secondary motion and
the camera fixed and exclude contacts, clip changes and feedback passes.

First compare the normal-cursor output to the existing pose and draw palettes
within a justified numerical tolerance. Then rasterize the half-tick pose into
one extra test frame through the same D3D11 materials. This establishes only
fractional pose evaluation/rendering, not a complete 120 Hz mode. Assert that
the extra sample does not change event counters, root position, RNG, guest
animation caches, simulation tick count or secondary integration count. Test
segment endpoints, absent tracks, mirroring, loops and rotation wrap with
synthetic/public fixtures; keep identity-matched real inputs and evidence private.

The runner owner can then perform a bounded under-one-minute natural scene gate,
checking distinct pose matrices and independently rasterized frames. The user's
play test of the named build remains the acceptance gate for visible correctness.
Next add the delayed timeline, moving camera, ball/contact policy, secondary
motion and dependent passes. No such experiment was run in this investigation.

Planning estimates for one engineer familiar with this runtime:

| Deliverable | Estimate, conditional on decoder and pose equivalence |
| --- | --- |
| One-character fixed-camera half-tick experiment | 3-7 focused days |
| Tagged packet replay with safe lifetimes and independent pacing | 4-8 additional days |
| Camera, contacts, transitions, secondary motion and pass coverage | 2-5 additional weeks |
| Useful integrated split-rate mode | Approximately 4-8 weeks, with route coverage the main uncertainty |
| Presenter-only transform fallback | Approximately 1-2 weeks for admitted scenes; broader coverage still needs identity/visibility work |

These are engineering estimates, not measured schedules. The main risks are
generated-entry fragmentation, shared scratch state, root-motion/event coupling,
rotation singularities, secondary geometry ownership, per-pass camera identity,
resource reuse and CPU/GPU cost at two renders per tick.


## Native decoder checkpoint

The first implementation covers the game-owned scalar track coefficient builders,
not the full sampling/blending cluster or the one-character rendering experiment.
[`animation_track.c`](../../../recomp-runtime/animation_track.c) supplies pure
value, tangent and curvature decoding, all four segment forms (Hermite, linear,
quadratic and constant), and a bounded immutable integer scalar sampler. Float
stores and double intermediates retain the generated runtime's rounding order.
Absent tracks have an explicit default. Backward samples start a fresh scan;
clip looping remains the caller's responsibility. This API does not substitute
scalar interpolation for the channel-aware fractional pose blend.

The [adapter](../../../recomp-runtime/animation_track_adapter.c) registers the
four coefficient builders through `recomp_lookup_manual()`. The default remains
generated behavior. `RECOMP_ANIMATION_TRACKS=1` enables the native builders;
`RECOMP_ANIMATION_TRACKS=verify` compares native coefficients byte-for-byte with
one normal invocation of the generated builder and stops on a mismatch. This
verification path preserves the original register and scratch-memory effects.
It is a game-owned diagnostic oracle, not a generated-library dependency in the
native model. Neither switch changes gameplay or presentation timing.

The numeric tests exhaust all 17,891,328 encodings across the three custom
number formats. An additional private harness linked the unchanged generated
source and found byte-identical decoded floats for every encoding and identical
coefficient bytes for 100,000 synthetic records of each segment form. Synthetic
public tests cover segment boundaries, backward reads, absent tracks, truncation,
and adapter memory/register boundaries. These results establish decoder
compatibility, not per-frame palette identity or visual pose correctness.

A bounded natural attract smoke compared 24,558 Hermite and 5,739 linear
segments over 693 reported frames with zero coefficient mismatches. The other
two forms were not observed in that window. A separate native-builder smoke
reached the character attract scene and closed normally. The median logged
presentation rate was 60 fps; this is not a 120 Hz measurement or user gameplay
acceptance. The later screenshot capture caused a substantial readback stall,
so its pacing should not be treated as an uncaptured rendering benchmark.
Per-frame palette comparison and the half-tick image remain unproven.

A dispatch constraint needs resolving before the complete cluster can be replaced:
direct calls in the current generated snapshot invoke generated symbols without
consulting `recomp_lookup_manual()`. In particular, registering `0x000AF050`
alone does not intercept its live callers. The four coefficient builders work
because the walker calls them through a function table. Do not rewrite the
generated callers by hand. Further integration needs lifter-supported direct-call
replacement, or a coherent hand-written caller that reaches the sampler through
manual dispatch. A lifter change must follow the existing pin/regeneration and
play-test rules; broadening into the whole-game timing owner is not part of this
split-rate change.

The remaining first-experiment work is pose-cache/mirroring equivalence,
channel-aware blending, scratch skeleton/IK and palette construction, and the
normal/half/next-tick rasterized comparison with unchanged gameplay state.
`RECOMP_SPLIT_RATE` is not implemented. No 120 Hz result follows from the scalar
decoder tests. Revised planning estimate: 4-8 focused days for the first visual
experiment, including dispatch integration; the integrated mode remains roughly
4-8 weeks, conditional on palette identity and render-pass coverage.


## Direct-dispatch checkpoint

The pinned lifter already supports selected direct-call and tail-call routing.
The smaller solution is to extend the authenticated manual-call selection, not
replace the owning game-update traversal. The regeneration tool now accepts
repeated `--manual-call-target` arguments, verifies the old generation first,
updates the selected-input hash, regenerates, and updates the recipe together.
The lifter revision remains unchanged. Failure restores both the selection and
recipe metadata. No generated C is edited.

The sampler, fractional sampling entry, blend dispatcher, skeleton assembler,
draw-palette builder and camera builder are now selected. Regeneration changed
14 function bodies in four source chunks, with no functions added or removed.
The new generation reproduced its authenticated manifest with Capstone 5.0.9.
`RECOMP_ANIMATION_DISPATCH_TRACE=1` observes these entries while calling each
original exactly once; normal execution remains the default. A bounded attract
smoke confirmed that direct calls reach the sampler, skeleton, palette and camera
adapters. The shutdown reported presenter status CLOSED while handling the
requested window close; this is recorded as a smoke limitation, not clean exit
or gameplay acceptance.

`RECOMP_POSE_CAPTURE` supplies a private output prefix for a bounded diagnostic
window. It records sampled channels, actor/limb state, executable-owned rig
inputs, and before/after bone matrices. Captures must remain under `private/`.
The raw records are diagnostic observations and do not define the pure solver
API. They must not be passed off as independently solved poses.

The next gate is a separate, immutable-input skeleton model covering body,
terrain correction, look-at, limb IK and derived slots. The exporter can reuse
that model's math only after its integer-pose comparison passes. No half-tick
rendering or split-rate presenter mode is admitted by this dispatch checkpoint.


## Pure skeleton model checkpoint

[`animation_skeleton.h`](../../../recomp-runtime/animation_skeleton.h) defines:

```c
bool recomp_animation_solve_skeleton(const RecompSkeletonTables *tables,
    const float channels[60], const RecompSkeletonTargets *targets,
    RecompBoneMatrix output[32]);
```

The model has no guest-memory, rendering, interception or generated-code
calls. Tables, channels and targets are immutable. It returns world-space
bone matrices; a rejected or singular input leaves output unchanged. The
caller supplies executable-owned rig tables privately. No rest-pose data is
embedded in the implementation. This API does not advance the controllers
that prepare its inputs at a simulation tick.

Conventions match the generic model-format description: row vectors,
translation in matrix elements 12..14, radians, and channel indices excluding
the three-word clip header. Local rotations are applied Z, then Y, then X
by premultiplication. Local translation adds the three basis rows, weighted
by the offset, to the translation row. The original seventh-order quarter-turn
sine approximation matters: substituting exact trigonometry changes the basis.
The native implementation derives that series mathematically and uses standard
C math for inverse trigonometry. Those choices require numerical comparison;
bit identity is not assumed.

| Assembly | Inputs and operation |
|---|---|
| Root 15 | Frozen actor position and heading about Y |
| Hip 2 | Root, local Y channel 1, Euler channels 3..5 |
| Spine 23, torso 0 | Offset 23 with channels 44..46; offset 0 with channels 6..8 |
| Shoulder bases 20, 16 | Torso plus offsets; Y then Z using channels 51/50 and 53/52 |
| Neck 19, head 1 | Offsets under torso/neck; Euler channels 47..49 and 9..11 |
| Eyes 21, 17 | Head plus offsets, Y then X from four frozen eye-controller channels |

Terrain heights are inputs sampled at the uncorrected hip, neck anchor and
four clip-driven limb targets. Hip clearance between -0.1 and 0.5 controls
body tilt; the horizontal neck-to-hip direction fades in over distance
0.1..0.2. The resulting shortest swing rotates hip, spine and torso about
the hip pivot before shoulders and head are assembled. The read-only diagnostic
adapter reads the original bilinear height grid to prepare these inputs.

Look-at transforms the target with the torso's transposed rotation after
subtracting translation, then subtracts neck and head offsets. X/Y are halved;
Z is clamped to at least 0.05, then replaced by `(Z+1)/2`. Pitch is limited
to -60..10 degrees and yaw to -60..60. Neck receives 30 percent and head
70 percent of these angles. Each is blended with the clip rotation by the
frozen look weight using a relative axis-angle rotation, converted back to
Euler angles, then assembled normally. Nonfinite look targets disable this
correction. The controller's weight ramp remains a simulation responsibility.

Each named 24-byte limb descriptor supplies the end, parent, proximal, middle
and optional tip slots, channel indices, signed fixed quarter turns, bend sign,
target-offset mask and morph group. The clip position is transformed by the
root. Its height is adjusted by terrain minus the actor's ground height; a
masked frozen target offset may then be added. The two pole channels describe
`(cos(a)*sin(z), -sin(a), cos(a)*cos(z))`, rotated by the root. From the
parent's translated proximal anchor, normalize the target direction as Z,
remove its projection from the pole to obtain Y, then set X to `Y cross Z`.

For distance `d`, link lengths `L1,L2` and stored squared difference `D`,
solve `alpha=acos(clamp((d*d+D)/(2*d*L1)))` and
`beta=acos(clamp((d*d-D)/(2*d*L2)))`. At distances no greater than 0.001,
both angles are pi/2. An optional minimum bend clamps each to 0.1 radians;
the descriptor controls their sign. Rotate the basis about Y by alpha for
the proximal bone; advance along local Z by L1, then rotate by `-beta-alpha`
for the middle bone. Descriptor quarter turns orient the stored matrices.
Advance by L2 and replace orientation with the root orientation before applying
the endpoint's clip Euler angles. The optional tip adds its offset and X angle.
A coincident target or collinear pole is rejected instead of publishing NaNs.

Morph weights mix the two offset tables and the lengths used in the angle
solve. The upper-body anchor rebuilds hip/spine/torso offsets before the
proximal offset; lower-body morph starts from the hip. The nominal proximal anchor and original link
lengths still control translation between stored joints. This asymmetry is
intentional and must survive a port of the solver.

Derived slots use the shortest swing between the source and destination
first basis rows. Slots 26/27 align 16/20 toward 8/14; slots 24/25 align 5/11
toward 4/10. Translation comes from the destination. Slots 30/31 are halfway
relative-axis-angle blends of 8/26 and 14/27. Slots 4/10 are blended halfway
toward 24/25; 24/25 then either copy those results or perform another halfway
blend, depending on the frozen flag. Slots 28/29 copy 24/25 orientation at
5/11 translation. Relative rotation uses a transpose, not a general inverse;
normalizing the approximate bases changes the original calculation.

[`animation_palette.h`](../../../recomp-runtime/animation_palette.h) separately
applies bounded draw recipes: load a bone, add/subtract a local rig offset,
terminate an output. Scratch carries between outputs. The mesh supplies its required output count, so an unused declared suffix
is not evaluated. It produces up to four matrices and fails atomically on
invalid indices or a truncated used prefix. These
are the matrices to compare with draw-time WORLD palettes, rather than assuming
all 32 bone matrices are sent directly to the renderer.

`RECOMP_SKELETON_VERIFY=1` invokes the generated tick once as a diagnostic oracle,
then independently solves immutable inputs and compares both bones and draw
palettes during a bounded window. Gameplay and rendered output remain generated
in this mode. The root, morph ramps, eye controller and look-at ramp are still
tick-owned and are not replaced by this checkpoint. This is a verification seam,
not evidence that the entire stateful B01E0 entry has been replaced.

A bounded natural attract smoke compared four actors at every simulation tick
from frame 1900 through 2019: 480 complete 32-matrix poses. All 2,604 observed
draw palettes (7,416 matrices, ten recipe IDs) were paired with a native pose
from the same actor and frame. Maximum absolute element error was
`6.9335103e-5` for both bones and draw palettes. Recipe evaluation from the
original bone bank had maximum error `1.90734863e-6`. No comparison was skipped.
A separate C invocation over the retained immutable inputs reproduced the
results, with unchanged inputs and byte-identical repeated native outputs.

The gate uses an absolute `1e-4` element budget for the independently implemented
math, not bit identity. For an affine palette acting on a vertex `(x,y,z,1)`,
this bounds each coordinate residual by `1e-4*(abs(x)+abs(y)+abs(z)+1)`; convex
skinning weights do not increase that bound. It is a numerical compatibility
gate for the observed window, not a guarantee for larger coordinates, negative
weights, near-singular configurations or every clip. Inverse-trigonometric
conditioning and intermediate rounding still limit wider equivalence claims.

The window had zero look-at and morph weights, the additional derived blend
disabled, and essentially level terrain. Those active branches have synthetic
coverage but no natural-scene identity result. Median logged presentation was
47 fps at the required rendering settings, with the verification and screenshot
readback enabled. Closing the window ended with presenter status CLOSED; this
is a smoke observation, not clean-exit or user gameplay acceptance. There is no
120 Hz measurement.

Synthetic tests exercise IK reach, world
translation, look-at, morph, terrain, derived blending, repeatability, input
immutability and atomic rejection. They do not establish natural-scene coverage
of active look-at, morph or sloped terrain. Half-tick rendering and
`RECOMP_SPLIT_RATE` are not implemented by this checkpoint. The remaining
half-tick experiment needs independent clip/mirror/blend sampling, a frozen
controller snapshot, tagged retained draws, and camera/pass binding. Planning
estimate: 3-6 focused days for that visual experiment; 4-8 weeks for useful
integrated split-rate presentation, conditional on broader branch coverage.


## Bounded half-pose replay experiment

`RECOMP_POSE_EXPERIMENT_FRAME=N` and `RECOMP_POSE_EXPERIMENT_ACTOR=0..3`
evaluate one actor between ordinary ticks N-1 and N. The native channel model
blends translations, relative Euler rotations, spherical directions and wrapped
angles using caller-supplied channel groups. It preserves destination mask and
unselected channels and applies the original hip-angle quantization. A private
comparison of 476 adjacent captured pairs against the original interpolation
routine found exact translations and maximum wrapped angular error
`9.590387e-5` radians, approximately one hip quantization unit. This is
interpolation between sampled poses, not a completed replacement for the clip
cache, mirror policy or all sampling entry points.

The experiment evaluates the skeleton again from those channels, frozen
controller inputs and interpolated visual root placement. It checks all 64 MiB
of guest RAM before and after evaluation; no guest writes occurred. Root-motion
updates, events and secondary integration still execute only at ordinary ticks.
The render worker replays one owned command packet with native palettes at
N-1, halfway and N. The camera and captured geometry remain fixed at N in all
three pictures to isolate the pose experiment. This is not a 120 Hz mode or a
one-tick-delay implementation.

Palette and rigid-object observers attach actor/object provenance to draws.
Rigid callbacks at 0x17D520 and 0x17D670 are temporary SDK-library seams: each
observer calls the original exactly once, and wholesale library replacement
remains open. The SDK queues copies of palettes for deferred passes, so bindings
must survive the callback and match the object plus exact original palette,
not a scratch-buffer address. Conflicting matches stop the experiment. Capture
packets own replay matrices, vertices, indices and texture data for the replay;
packets containing a resource-release record decline the experiment. Recomputed
WVP, lighting-normal and reflection transforms leave the source command intact.

The visible-actor smoke produced the requested three pictures and tagged 96
draws after adding rigid and deferred bindings. All 53,760 tagged vertex
instances were inside the clip volume; no backend draws were declined. The
retained packet occupied 13,443,304 bytes. Median logged ordinary presentation
was 60 fps at scale 4.5, MSAA 8 and SMAA, with vsync. Window close exited normally.
These are smoke observations; they establish neither gameplay acceptance nor
120 Hz pacing.

**The visual gate failed.** N-1 and the midpoint still have elbow/wrist seams;
N is coherent. The initial weighted-only replay also detached rigid parts, and
a callback-lifetime binding missed deferred passes. Fixing those two binding
errors improved the image but did not remove the remaining seams. A separate
bounded diagnostic paired 499 draw calls across the two ordinary ticks by
object, layout and identical indices. Referenced vertex data changed in 79
pairs, including the selected actor's body and attachment draws. Comparing only
palettes is therefore insufficient for this scene. Raw captures and screenshots
remain private. `RECOMP_POSE_VERTEX_CAPTURE` optionally writes these two ticks'
vertex/index records to a private output prefix for further diagnosis.

The next prerequisite is an independently callable pose-dependent geometry
stage with immutable secondary-state inputs. The 0x56E90 owner mixes simulation
and geometry work: 0x57300 advances secondary state, whereas 0x57250 dispatches
geometry writers 0x57660/0x57CB0; other deformation owners also occur below that
owner. Do not rerun the entire owner at presentation frequency. Trace the
remaining seam draws to their geometry writers, compare ordinary-tick geometry,
then evaluate it with the new pose and frozen physics inputs. Do not hide the
failure by interpolating final matrices or by silently omitting attachments.

`RECOMP_SPLIT_RATE` remains unimplemented because its prerequisite visual gate
has not passed. Camera evaluation, all-actor coverage, packet delay/lifetime,
cut/topology invalidation and measured 120 Hz pacing are also outstanding.
Revised planning estimate: 1-2 focused weeks to resolve the geometry gate, then
4-8 weeks for useful integrated split-rate presentation. The breadth of the
secondary/deformation owners makes that estimate uncertain.


## M1: clean same-tick replay

`RECOMP_REPLAY_VERIFY_AT=N` compares the ordinary captured packet for each of
120 consecutive simulation ticks with a second execution of that same packet.
The packet owns every submitted vertex/index span and all sampled texture
payloads before the producer can advance. Thus CPU skinning, derived-joint,
stitch, face and secondary-motion vertices retain their own tick's values,
alongside that tick's palettes, camera and draw state. No pose substitution or
half-tick evaluation participates in this gate. Resource-release packets and
mixed pose experiments are rejected rather than silently skipped.

Comparison reads the final 3840 by 2160 back buffer after MSAA resolve, SMAA and
output rendering, before each flip. RGB pixels are compared directly; alpha
padding is excluded. A mismatching pair is logged and stops verification. Two
natural attract windows, ticks 4800..4919 and 6500..6619, each passed all 120
comparisons with zero changed pixels and zero maximum channel difference.
No noise allowance was needed. Each window compared 995,328,000 pixels at
scale 4.5, MSAA 8, SMAA enabled and vsync. Host input was disabled; only the
two deterministic startup pulses were supplied. Both runs exited normally.
Screenshots confirm the intended match and close-up scene windows. These are
agent smoke and pixel-identity observations, not gameplay acceptance.

This corrects the earlier experiment's ownership assumption: a packet is a
completed tick, not pose-independent geometry. Its N-1 picture had combined
N-1 palettes with N vertices and was not a replay of the real N-1 frame.
The capture layer owns all final draw payloads without depending on which
CPU writer produced them; the following reconstruction work must preserve
that same ownership boundary.

The earlier two-tick capture identifies changing referenced rigid geometry
near slots 4/5 and 10/11, alongside unchanged rigid controls using the same
slots. Deferred head passes are a separate binding path. The secondary owner
also mixes geometric output with history, velocities and collision updates.
Reconstructing those writers from immutable inputs is M2 work; copying one
tick's completed vertices proves M1 but does not prove fractional evaluation.

For the remaining milestones the target rate is a numeric value supplied to
`RECOMP_SPLIT_RATE`, not a boolean or a fixed double-rate mode. Presentation
must sample an arbitrary fraction in [0,1) of the 60 Hz simulation interval.
M2 additionally checks quarter and three-quarter poses. M3 retains the 120 Hz
gate and adds 240 Hz measurements at reduced settings and at 2160p/MSAA 8/SMAA;
the latter measurement is not required to pass. M2 through M4 remain open.


## M2: fractional pose and seam geometry

The bounded pose experiment now accepts a continuous interpolation fraction;
its diagnostic batch renders n, n+0.25, n+0.5, n+0.75 and n+1. Five is a
comparison-image count, not a presentation-rate assumption. The camera stays
fixed to isolate the selected character. The completed close-up and match
comparisons have continuous elbows, wrists and forehead attachments at all
five samples. The initial match selection was off-screen and was excluded.
The visible match actor exposed a separate unbound forehead attachment; its
actor-owned descriptor now supplies the bone identity for deferred draws.

The principal joint vertex owner is 0x67540, with workers 0x67630/0x67CA0.
An offline invocation of the original writer reproduced all six close-up arm
draw buffers byte-for-byte, both from their own captured bones and from the
preceding tick's bones. The readable `animation_seam` model reconstructs the
same position/normal data with maximum errors below 0.00000082 in the close-up
and 0.00000030 in the match. These are numeric comparisons, not bit identity.
The sampled draw buffers are host-owned and captured before their allocation
is released; replay does not write gameplay memory or advance secondary state.

For model exporters, the seam algorithm uses row-vector affine matrices:

- Form `source_bone * inverse(destination_bone)`.
- Read ten rig-provided float4 controls: three direction pairs, followed by
  four origin controls. For each direction pair, retain the first vector,
  transform the second as a direction, and normalize their sum. The origin
  basis retains the first origin, transforms the second, and averages the
  transformed third with the fourth.
- Each rig-provided coefficient triple mixes the three basis rows separately
  for X, Y, Z and origin. Normalize Z; remove Y's projection on Z and normalize
  Y; remove X's projections on Z and Y and normalize X.
- Transform immutable source positions and normals by that frame, then scatter
  xyz/normal triples to the mapped seam vertices. Preserve UVs and other
  attributes. The descriptor's destination bone is the draw's WORLD matrix.

The input stream starts vertex records after the ten controls (160 bytes).
A vertex record contains position float4 and normal float4. Command values
below 16 select a caller-supplied coefficient row; larger values reuse the
previous frame. Destination lists and groups have separate all-ones sentinels;
a zero command ends the stream. No retail controls or coefficient table are
embedded in the model. Optional copy/transform link streams were absent in
these windows and currently stop the diagnostic if encountered.

Rigid attachments dispatched by 0x58A50 use a separate actor-owned descriptor
list and SDK callback. Its mesh selector and bone byte establish provenance;
an exact current WORLD match is additionally required. This covers the tested
forehead attachment without guessing ownership from draw order or palette
similarity alone. General secondary geometry remains frozen at the ordinary
tick, and its integration owners are never rerun by this experiment.

For all 32 bones in each tested pose, the three interior samples have positive
orientation determinants, remain within the endpoint angular spans, and move
forward between endpoint translations. The largest quarter-step axis rotation
was 0.14 degrees in the close-up and 2.61 degrees in the match, versus endpoint
spans of 0.55 and 9.86 degrees. This is a geometric between-endpoints check:
curved rotations are not component-wise linear matrix interpolation. Individual
matrix components exceed their endpoint boxes by up to 0.001043 in the match;
clamping them would deform the recovered rotation algorithm. No flips or pops
were observed. Full guest-memory comparisons remained unchanged for evaluation.

Both windows produced all five images at scale 4.5, MSAA 8, SMAA and vsync with
host input disabled. The match run exited normally with zero backend draw
declines. The close-up capture completed, but its delayed window close hit the
existing presenter-close error path; this is not a clean-exit claim. Private
captures retain the images and numeric checks. Twenty CTest checks and the
full tools suite passed; public export verification passed. These are bounded
agent smoke observations, not user gameplay acceptance. M3 and M4 remain open;
`RECOMP_SPLIT_RATE` is not enabled by this milestone.


## M3: numeric split-rate presentation

`RECOMP_SPLIT_RATE=120` enables a target present rate while gameplay keeps its
ordinary 60 Hz clock. Other numeric rates from 60 through 1000 are accepted,
including 100, 144, 240 and fractional rates. Missing, zero or invalid values
disable it; the old proposed boolean value `1` does not enable this mode.
Do not combine it with the separate whole-game high-rate setting.

The worker owns the completed packet, clip-channel endpoints, skeleton tables,
actor targets, palette recipes, seam inputs and camera endpoints. It samples
the actual elapsed fraction of the 60 Hz interval at each present, clamped to
[0,1). There is no midpoint or double-rate assumption in evaluation. Presentation
has one tick of visual delay. Gameplay root motion, events, collision, IK target
updates and secondary simulation continue at 60 Hz; only visual root position,
heading and the sampled pose are evaluated between endpoints. The plain
`recomp_animation_visual_sample` model has no guest memory or dispatch access.

All tagged actors use the native solver and regenerate their joint seam vertices.
Camera eye/target, roll and projection inputs are interpolated, not extrapolated.
Camera history is invalidated on nonconsecutive ticks or changed mode flags.
The native camera model preserves the steep-view quantized heading branch and
approximate rotation arithmetic. An offline comparison of 10,001 camera cases
had maximum matrix error below 0.00000191. The main ball position and wrapped
rotation angles are interpolated; its ground shadow follows position. Historical
trail draws remain at their ordinary tick. Explicit object provenance plus an
endpoint WORLD match distinguishes the current ball from shared trail meshes.
The two captured ball endpoints matched the original WORLD matrices bit-for-bit.
HUD and screen-space draws are replayed once per present without pose transforms.

Longer runs exposed a nearly straight arm whose small normalization differences
amplified the IK bend angle. Packed offset transforms, packed rotations and the
original reciprocal-square-root refinement now preserve the relevant rounding.
The failing pose's maximum bone error fell from 0.0001367 to 0.00000218; the
previous 480-pose integer comparison now has maximum error below 0.00000335.
A synthetic near-straight-arm regression covers this independently of private
capture data. Runtime palette and camera comparisons still stop on a mismatch;
tolerances were not relaxed.

A capture-free 120-second measurement at 3840 by 2160, MSAA 8 and SMAA recorded
14,399 presents: 119.992 fps, with 120 fps median one-second samples and
8.334 ms mean reported frame time. Gameplay capture remained 60 Hz. This is
within one present of the requested count, not a claim of zero scheduling jitter:
the longest interval was 20.3 ms and the slowest one-second sample was 118 fps.
The earlier scheduler lost 31 presents by discarding deadlines just before late
packet publication; retaining those due deadlines removed that systematic loss.
The complete 150-second smoke exited normally with zero backend draw declines.

Two additional 60-second runs measured the same attract route. After a 20-second
warm-up, scale 2 with MSAA off and SMAA (1707 by 960) measured 239.949 fps and
4.168 ms per present. At 3840 by 2160 with MSAA 8 and SMAA, the measurement was
239.872 fps and 4.169 ms per present. Their longest intervals were 13.8 and
16.4 ms. These are observed host results, not universal 240 Hz guarantees.
Host input was disabled for every timing run.

Synchronous match captures showed coherent arms, attachments, camera and ball
motion, with a single stable HUD. Their readback stalls disturbed sampling, so
they establish image content rather than intermediate-present cadence. M4's
deferred captures below supply that cadence evidence. Screenshots and detailed
pacing logs stay private. Twenty-one CTest checks and the full tools suite cover
the new path.

Limits: secondary geometry remains frozen at its captured simulation tick;
optional seam link streams still stop explicitly if encountered. General teleport,
rig-topology and scene-cut coverage beyond the observed route remains unproven.
The measurements are agent smoke results, not user gameplay acceptance. M4's
controlled-input match and 300-present inspection are the remaining play-test gate.


## M4: bounded play-test readiness

The local play launcher accepts a numeric present rate, defaults to 120, and
accepts `off` for ordinary presentation. It selects scale 4.5, MSAA 8, SMAA and
vsync; the interactive route enables host input after the startup pulses. Timing
and controlled smoke scripts keep host input disabled. All runs respect the
machine-wide single-runner rule. The launcher and run artifacts remain private.

The controlled route uses the ordinary Exhibition menus, character selections
and match inputs. It exposed a post-skeleton net-contact correction at 0xAE4A0:
the original game shifts every bone's world-Z translation after 0xB01E0. Replay
now captures this completed 60 Hz displacement and interpolates its visual
contribution after solving the pose. It does not rerun collision or move gameplay
state. The adapter verifies all other matrix components remain bit-identical to
the captured original post-skeleton bank, and verifies that every bone received
the same Z shift. An unexplained post-solve transform still stops the run.
For exporters, this is an optional uniform world-Z addition to all 32 output
translations; it is separate from the rig/IK solver. The failing controlled
pose then matched the final bone bank within 0.00000647, without relaxing the
palette gate. Synthetic coverage checks fractional displacement independently.

Synchronous BMP output was too intrusive for a cadence check: per-pixel stdio
and readback waits biased samples toward tick boundaries. The writer now emits
whole rows. For bounded motion evidence, `RECOMP_D3D_FRAME_DUMP_DEFER=1` queues
owned staging copies and reads them after the requested frames were presented.
The diagnostic caps the burst at 300 frames and 12 GiB of raw storage; a 4K
burst uses about 10 GB. Saving pauses the runner after the burst, so this is not
a pacing benchmark. A graphics test verifies ownership and order using three
successive, distinct clear colors. Default rendering does not allocate this queue.

The final attract and controlled-match captures each contain 300 consecutive
3840 by 2160 presents at the 120 Hz target with MSAA 8 and SMAA. Their captured
time spans were 2.515 and 2.656 seconds; allocation/copy overhead remains visible.
Both runs exited normally with zero backend draw declines and no palette or
camera mismatches. All 299 adjacent pairs in each sequence differ, including
odd presents. Contact-sheet and full-frame inspection found no seam breaks,
attachment separation, HUD doubling or flicker. Camera and ball are interpolated.
These are bounded agent smoke observations, not a user gameplay acceptance claim.

A final capture-free 120-second window recorded 14,401 presents (120.008 fps),
with 120 fps median samples, 8.333 ms mean reported frame time and 60 Hz gameplay
capture. The maximum interval was 24.0 ms; this is not a zero-jitter guarantee.
The full 150-second run exited normally. The earlier 240 Hz measurements remain
as reported in M3. All 21 CTest checks and the tools suite (21 tests, one skip)
passed, and public export verification passed.

The requested bounded gates are complete and the local build is ready for the
requested user play test. Optional seam-link streams, wider secondary-geometry
coverage and uncommon cuts/teleports remain limitations; their guards remain
active. Revised estimate for broader release coverage is approximately one to
two focused weeks, depending on what those additional scenes expose.


## S1: cuts and teleports

Interpolating across a discontinuity would draw an invented in-between frame,
so split replay now shows those ticks unblended. Each 60 Hz tick compares its
step with the previous tick's step. A step more than eight times the previous
one, plus a small floor, is a discontinuity. Cameras use eye/target travel
relative to view distance and view-direction change. Actors use the root bone's
translation and rotation. The main ball uses its position step, so serves and
resets also snap. Nonconsecutive ticks and changed camera mode flags already
invalidated history. Menus and FMV frames have no tagged draws and replay
unchanged. After a jump, the next tick compares against the jump's real step,
so continued fast motion is not flagged again.

The thresholds come from a 120 Hz trace of the complete attract cycle. Every
shot change scored at least 20 times its preceding step; continuous motion
scored at most 4. `RECOMP_SPLIT_TRACE=1` logs per-tick camera and actor steps
and, for each present, its fraction and the number of camera- and pose-blended
draws.

A second full cycle with the detector active (38,784 presents, ticks 1 to
17,271, scale 4.5, MSAA 8, SMAA, host input off) contained 38 camera cuts.
Thirty-six were flagged by the step test. Two (ticks 1848 and 8657) followed
nonconsecutive ticks. The presents that span each cut tick had zero
camera-blended draws. Seven scene entries after loads also replayed
unblended. The detector reported no cuts that the offline scan had not found.
Twenty-seven actor root jumps were snapped, including all four actors at the
shot changes 3193, 5892 and 8195. The run exited normally with zero backend draw
declines. Cut ticks and their present numbers in this run:

| Tick | Presents | Tick | Presents | Tick | Presents |
| --- | --- | --- | --- | --- | --- |
| 1848 | 3707-3708 | 3773 | 7557-7558 | 6315 | 12641-12642 |
| 1963 | 3937-3938 | 4053 | 8116-8117 | 6815 | 13641-13642 |
| 2033 | 4077-4078 | 4293 | 8596-8597 | 6925 | 13861-13862 |
| 2133 | 4277-4278 | 4716 | 9442-9444 | 6965 | 13941-13942 |
| 2373 | 4757-4758 | 4963 | 9937-9938 | 7095 | 14201-14202 |
| 2553 | 5117-5118 | 5333 | 10677-10678 | 7145 | 14301-14303 |
| 2903 | 5817-5818 | 5491 | 10993-10994 | 7685 | 15382-15383 |
| 3108 | 6226-6227 | 5575 | 11161-11162 | 7765 | 15542-15543 |
| 3193 | 6396-6397 | 5688 | 11388-11389 | 7945 | 15902-15903 |
| 3308 | 6626-6627 | 5838 | 11688-11689 | 7987 | 15986-15987 |
| 3443 | 6897-6898 | 5892 | 11796-11797 | 8030 | 16072-16073 |
| 3558 | 7127-7128 | 6015 | 12041-12042 | 8195 | 16402-16403 |
| | | | | 8435 | 16880-16881 |
| | | | | 8657 | 17324-17325 |

A snapped tick holds the previous visual state for one 60 Hz interval, which is
exactly the ordinary presentation. A fast real turn can occasionally exceed the
ratio (for example a 24-degree root turn at tick 1999). That costs one
unblended tick and causes no visual error. The ratio test is a heuristic. A cut
between two nearly identical shots would not be detected, but it would also
look the same either way.


## S2: present hitches

The 24 ms interval reported for M4 had four reproducible causes. A
buffered per-present trace (`RECOMP_SPLIT_TRACE=present`, written at exit or
after `RECOMP_SPLIT_TRACE_LIMIT` presents) records each present's deadline,
start, end, packet publication time, fraction and draw count without writing
to stderr while pacing.

1. The game thread sealed each packet while holding the presenter queue
   mutex. Sealing copies the owned textures, about 23 MB per tick and more
   while the game streams assets (ticks 4809 to 4811 of the attract cycle).
   The worker checks that mutex before every present, so a slow seal delayed
   presents by up to 11 ms. The open slot is invisible to the worker until it
   is counted as pending, so sealing now happens outside the lock.
2. At some cuts the game publishes the next packet 8 to 13 ms late. The worker
   used to idle until it arrived. After the current tick's presents, it now
   presents the current packet again at its end pose at each deadline. This
   is the same image ordinary 60 Hz presentation would show.
3. A new shot creates 50 to 143 textures, 4 to 13 ms of work, inside its first
   present. Packets arrive just in time, so there is no lookahead. The worker
   now keeps holding the current packet while it creates the next packet's
   missing textures in idle time before each deadline. It stops 1.5 ms short
   of the deadline and waits at most one tick. Textures estimated to take
   longer than one present slot are left to their draw. Only addresses with no
   cache entry are prepared, so entries the packet on screen uses are never
   replaced.
4. With two flip buffers, `Present` blocked for 7 to 33 ms whenever a
   compositor frame was late. Split mode now uses three buffers. Ordinary
   presentation keeps two.

Runs used the attract route with host input off, scale 4.5, MSAA 8 and SMAA at
120 Hz, measured from 35 s to 155 s after the first present. Before the
changes, one window had a 35.6 ms maximum and 13 intervals over 12.5 ms. Three
windows on the final binary:

| Run | Presents | Rate | p99 | p99.9 | Max outside loads | Over 12.5 ms |
| --- | --- | --- | --- | --- | --- | --- |
| 120 a | 14,399 | 119.990 | 9.19 ms | 10.41 ms | 23.85 ms | 4 |
| 120 b | 14,396 | 119.963 | 9.45 ms | 10.95 ms | 29.92 ms | 3 |
| 120 e | 14,394 | 119.949 | 9.58 ms | 10.60 ms | 23.09 ms | 3 |

The only load in each window is the first frame after the blank loading
packets at the end of the attract demo (tick 8662). It took 16.7, 16.8 and
42.6 ms. At 240 Hz with scale 2, one window recorded 28,760 presents
(239.661/s), with a mean of 4.173 ms, p99 of 5.53 ms, p99.9 of 7.33 ms and a
13.28 ms maximum. Eighteen intervals exceeded 8.5 ms.

**The S2 gate is not met.** The remaining outliers are single slow presents
that do not repeat at the same ticks across runs. GPU load stayed at 33 to 39%.
Per-process sampling showed other desktop programs active at those moments,
for example Discord at 118 to 160% CPU during two of the stalls. Raising the
worker to MMCSS "Games" scheduling and raising the D3D GPU thread priority did
not change the outlier count, so neither was kept. A quiet machine, or a
measurement that excludes host interference, is needed to show whether
anything in the runtime still causes isolated stalls.


## S5: game-thread capture cost

Each tick the game thread copies its draw data into a presenter packet. The
span index that deduplicates those copies was an `unordered_multimap` that
allocated on every insert and was freed with every packet. It is now an
open-addressed table owned by the packet slot. `clear()` starts a new
generation, so a slot allocates nothing once its table has grown to a tick's
span count. The animation probe also read its diagnostic settings with
`getenv` on every dispatch and draw; it now reads each setting once.

Runs on the Exhibition match route (scale 4.5, MSAA 8, SMAA, host input off)
averaged pacing seconds 80 to 147 and alternated the two builds:

| Build | Split | Game thread | Copy (`add`) | Seal |
| --- | --- | --- | --- | --- |
| before | off | 8.22, 8.03 ms | 1.00, 1.03 ms | 0.02 ms |
| S5 | off | 7.99, 8.46 ms | 0.91, 0.97 ms | 0.02 ms |
| before | 120 Hz | 9.60, 10.07 ms | 1.45, 1.42 ms | 1.04, 1.11 ms |
| S5 | 120 Hz | 9.58, 9.17 ms | 1.26, 1.32 ms | 0.91, 0.91 ms |

Copy and seal each fell by about 0.15 ms at 120 Hz. Game-thread totals vary
by up to 0.5 ms between runs of one build, so the total gain is within noise.
The remaining copy is mostly vertex and index bytes the game rewrites each
tick; borrowing them from guest memory would race with the next tick.

Two attract runs of the unchanged build, dumped at ticks 1800, 3600 and 5400,
differed by 0.73, 0.27 and 0.12 mean luma levels, with 2.1%, 1.1% and 0.3% of
pixels differing by more than 8. The S5 build against each of them differed by
0.61 to 0.70, 0.14 to 0.29 and 0.11 to 0.16, with 1.6 to 1.9%, 0.5 to 1.2%
and 0.3 to 0.7%. The S5 image differences are within that run-to-run noise.

One 120 Hz S5 run ended with `d3d-clear:presenter:9` (presenter closed)
when the script closed the window at 150 s. The game thread submitted a
Clear after the presenter had shut down. That is an exit-ordering race in
the frame adapter, not an in-play failure.


## S3: pool hopping, shops and the match

Pool hopping stopped with `split:ambiguous-binding`. With the binding
details logged, a 120 Hz run showed one girl (actor 0, one object, four
palette influences) building three palettes per tick with the same recipe,
offsets and output matrices. Only the `initial` matrix differed, so the
game draws her three times per tick with different pass transforms, most
likely the girl and her pool reflections. Each draw builds its palette just
before drawing, so the newest matching binding belongs to the current draw.
The draw lookup now searches newest first and logs the first 16 ties instead
of stopping.

Agent smoke runs at 120 Hz, scale 2, MSAA 8 and SMAA, checked 240-present
bursts with `s3check.py`, which flags blocks that change only every second
present (60 Hz stepping), single-present flicker, pops and repeated frames:

| Scene | Moving blocks | 60 Hz stepped | Flicker presents | Pops | Repeats |
| --- | --- | --- | --- | --- | --- |
| Pool hopping | 300 | 83 | 4 (burst end) | 7 | 0 |
| Sports shop | 111 | 50 | 0 | 0 | 0 |
| Exhibition match | 1007 | 0 | 0 | 0 | 0 |

The pool-hopping flicker presents and pops are in the last ten presents of
the burst and cover the full frame, which matches a shot change. The 60 Hz
steps in pool hopping are spread over the water. In the shop they sit in one
region at a ratio of 1.0, which fits the rotating item preview. Neither
object is an actor or a single-bone rigid draw, so it gets the interpolated
camera but keeps its 60 Hz motion. Both runs continued without a stop.
