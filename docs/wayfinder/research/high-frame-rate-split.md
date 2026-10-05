# Split-rate animation and camera rendering

Design: 2026-10-03. Decoder checkpoint: 2026-10-05. Split-rate rendering
is not implemented or play-tested.

## Recommendation and evidence limits

Keep gameplay, root motion, physics, AI, input consumption, animation events and
stateful effects at 60 ticks/s. Evaluate a separate visual pose and camera twice
per tick and rasterize both frames through D3D11. The game already has fractional
animation sampling, but its entry point also changes root-motion state. Calling
the existing update twice is not a safe implementation.

Prefer replay of a tagged, resource-safe draw packet over a second invocation of
the character draw/update traversal. The existing packet is a useful transport,
not yet a reusable scene description. Its matrices have lost bone/camera identity,
some resources are borrowed, and the worker clears it after execution.

This investigation used bounded read-only Ghidra queries, the configured local
generated program, the tracked runtime, and a separate private model/animation
survey. No runner was launched and no visual result is claimed. Ghidra image
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
