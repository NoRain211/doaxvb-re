# Render pipeline at the native runtime boundary

Source review and capture survey: 2026-10-03, local `codex/rtgi-dxr`.
Rendering baseline: `96d2baa`; capture instrumentation: `ecc68ec`.
This describes the intercepted D3D8 calls and their D3D11 delivery. It is not
an inventory of every original Xbox GPU submission. Raw records and images
remain private; resources below are described by role, never asset filename.

## Scope and capture contract

`recomp_lookup_manual()` routes into the creation, frame, state, texture and
draw adapters. Indexed and UP draw adapters still call their generated driver
bodies for push-buffer/fence bookkeeping, then independently construct host
commands. This existing library seam remains open work for wholesale D3D8
replacement. Retained device state supplies transforms, light records, stages,
shader handles and attachments; state models supply effective blend/depth/stencil
overrides. The presenter packet copies commands and selected payloads, and the
render worker executes them in submission order.

Sources: [indexed adapter](../../../recomp-runtime/d3d_draw_adapter.c#L1611),
[UP adapter](../../../recomp-runtime/d3d_draw_adapter.c#L2216),
[packet copy](../../../recomp-runtime/d3d_presenter_capture.cpp#L34),
[worker](../../../recomp-runtime/d3d_presenter_thread.cpp#L94).

Set `RECOMP_D3D_CAPTURE_HOTKEY=1` and press F11 with the runner focused. The
request is consumed after Swap and captures the following complete frame.
F9/F10 can hold/step the game; a request while paused needs a resume/step to
reach that boundary. `RECOMP_D3D_CAPTURE_AT` alternatively accepts comma-separated
one-based present numbers. Neither option changes guest scene state.
`RECOMP_D3D_CAPTURE_ROOT` chooses the private output directory; the default is
`private/captures` relative to the working directory. Each present gets its own
subdirectory, and an existing subdirectory is refused rather than overwritten.

- `calls.jsonl`: ordered intercepted draws, clears and gamma commands, with
  effective state snapshots. Rejected draws stay in this list. `call` is the
  join key; `accepted` means enqueued, not rendered. There is no draw-row cap
  in whole-frame mode. Existing geometry bounds still have an explicit scan
  budget; incomplete bounds do not mean omitted calls.
- `host.jsonl`: worker outcomes under the same call IDs. Draw records include
  host path, exact backend rejection category, texture sampling success,
  dropped shared depth and effective host viewport. Non-draw records contain
  command type and result; a successful PRESENT is the completion marker.
- `frame.png`: opaque screenshot of the final swapchain image before Present,
  after host resolve/output processing. No texture texels are included in JSON.

`render_targets` is current color, current depth, default color, default depth.
Descriptors include storage identity, format, size, pitch and mip count.
`viewport_words` contains guest X/Y/width/height plus float-bit min/max depth.
`texture_stages` holds all four bindings and 32 state words per stage, including
bump matrices. `render_state_words` holds the retained render-state block,
including combiner structure, constants, fog and point state. Interpret it
alongside `simple_overrides`, whose pairs are encoded method/value: setters can
override retained words. The draw's original pixel handle and FVF/program handle
are also retained. This captures state at use, not a chronological log of every
setter or unused target switch. Resource IDs are meaningful only within a run.

The known retained layout agrees with the public
[Cxbx render/texture state definitions](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/XbD3D8Types.h).
A bound higher texture stage does not establish that its disabled operation was
used. State words from fixed-function draws can contain stale combiner values;
only an active pixel handle establishes a programmable pixel path.

## Observed scenes and pass order

Twenty whole frames were captured in one natural attract run, from startup
through the jungle/Christie sequence. All **12,849 draws** were accepted by the
adapter and drawn by D3D11; every ordered call ID matched its worker record.
No draw rows were dropped. All submitted primitives were triangle strips;
no active pixel shaders or vertex programs appeared. This says nothing about
uncaptured hotel, shop, casino, pool or gallery rendering.

The main guest color target is 720x480 linear BGRA with default D24S8 depth;
the configured host main viewport is 3840x2160. Every observed offscreen color
target is swizzled BGRA, single-level, and explicitly has **no guest depth target**.
None hit the host's shared-depth-dropping fallback. Target switches below are
inferred from the attachment on each draw/clear, including switches that occur
and return within one frame.

| Captured view | Draws / clears | Ordered attachment/presentation structure |
| --- | --- | --- |
| Startup, present 2 | 0 / 1 | Main clear and gamma updates, then present. |
| Intro video/fade, 30 | 2 / 1 | Main clear; full-screen UP quad samples mutable 720x480 linear BGRA; untextured blended fade quad; present. The screenshot is near black at this transition, so it does not validate movie image quality. |
| Title/logo fade and island flyover, 60, 120, 240, 360, 480, 600, 1200, 1800 | 1,164–1,228 / 1 each | Main clear; background/world meshes, environment/reflection materials and translucent layers; pretransformed title/copyright/fade overlays; present. No offscreen pass in these frames. |
| Attract transition, 2400 | 395 / 3 | Main clear + 43 draws; 256x256 color clear + 35 draws; 512x256 color clear + 50 draws; return to main for 267 draws including one sample of the second target; full-screen transition and corner logo. Final screenshot is black under the transition. |
| Sunset close-up, 3000 | 238 / 3 | Main clear + 49 background draws; 256x512 color clear + 46 draws; return to main for 140 draws, including two meshes sampling that target; clear 256x256 and copy the current main color into it; depth-tested full-screen composite back to main, then corner logo. |
| Sunset volleyball attract match, 3600 | 461 / 2 | Main clear + 35 background draws; the same 256x512 offscreen pattern with 46 draws; main scene/characters, stencil layers and translucent effects; logo. No final scene-copy pass in this frame. |
| Daytime beach pose and match, 4200, 4800 | 194, 609 / 1 | Entire scene stays on main; world, characters, paired stencil shadow geometry, blended layers and final logo. |
| Jungle close-up, 5400 | 225 / 1 | Main-only world, character, foliage/translucent layers and logo. |
| Jungle match, 6000 | 491 / 1 | Main-only world/characters, stencil shadow pairs and logo. Three draws select a reflection-like stage pattern but miss reflection admission. |
| Christie jungle views, 6300, 6600 | 254, 217 / 1 each | Main-only world/character, 19 stencil/darkening pairs per view and logo. |
| Later transition, 7200 | 247 / 3 | Main; 256x512 color pass (46 draws); main; 256x256 scene copy (one draw); main full-screen composite/fade and logo. Final image is black under the transition. |

### Effects that can be identified at this boundary

**Shadows.** The visible character shadow and repeated geometry/state pairs
support a stencil-assisted projected-shadow technique. In the daytime beach
and Christie views, each of 19 pairs first disables color writes, compares the
low two stencil bits against reference 1 with LESS_EQUAL and inverts those bits
on pass. The matching draw enables color, tests EQUAL, blends a translucent black
texture factor and clears the matched stencil bits. Both use the same geometry
and main depth surface. This is stronger evidence than labeling all stencil
writes as shadows: many ordinary character/world draws merely replace stencil
with 0 or 254. No separate shadow depth map is sampled in this survey. Exact
caster/receiver membership still needs isolation before replacing these shadows.

**Blur/DOF.** The sunset close-up copies the finished main scene to a 256x256
color texture with a two-triangle UP draw. A full-screen UP quad samples that
smaller image back onto main with alpha blending, LESS_EQUAL depth testing and
depth writes enabled; its screen-space Z is about 0.911. This is a concrete
simple depth-gated blur/DOF candidate, not a demonstrated general depth-texture
or separable multi-pass blur implementation. The corner logo is drawn afterward
with depth disabled. No four-UV/four-tap filter was submitted. Downsampling the
scaled main buffer also uses the host's generated mip chain; the effective
sampling therefore includes a host approximation.

**Water/reflection.** The title flyover uses the admitted two-texture environment
reflection material. Later scenes also render geometry into rectangular color
targets and sample those targets on shallow world meshes, consistent with planar
water/reflection surfaces. The 256x512 target is sampled by two such meshes in
the sunset close-up and three in the sunset match. Their UV/material path is
fixed function; no programmable water draw or bump texture operation appears.
The capture proves the producer/consumer chain, but a reflected-camera or isolated
surface comparison is still needed to label every rectangular target a reflection.
The 256x256 target produced earlier in the 2400 transition is not consumed by an
admitted active texture stage in that captured frame; its later use is unknown.

**Sky, particles, sand and HUD.** Background/world draws precede character layers,
while the logo and fades are late XYZRHW quads. The broad near-horizontal title
meshes that request MULTIPLYADD are surface/water candidates; the ADD layers are
smaller elevated geometry. None is an identified bump-mapped sand pass. Blended
triangle layers and additive blends are present, but no point-sprite state or
point primitive is submitted. These may include particles, foliage, spray, sand
or other overlays; their individual screen footprints are not established by
state alone. Title/copyright and corner-logo graphics are observed; a general
font/text glyph pass and interactive menu UI remain outside this survey.

## Host presentation after the game passes

The main target uses the configured resolution scale and MSAA. Offscreen color
targets retain guest dimensions and one sample. Before final output the host
resolves main color, optionally applies SMAA (edge, weights, neighborhood blend),
and applies the gamma/output pass. Opaque main-target shading has ordered
8-bit dithering. These are host enhancements, not game-requested passes.
The survey uses scale 4.5, MSAA 8 and SMAA, with RTGI disabled.

Sources: [target binding](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L1146),
[output](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L3243),
[Present](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L3333).
When enabled separately, RTGI collects a restricted subset of main-target
geometry, applies before the first main-target pretransformed draw or at Present,
and does not replace the original offscreen pass chain.

## D3D8 feature use versus implementation

| Feature | Captured use | Host behavior |
| --- | --- | --- |
| Fixed-function vertices and skinning | All draws; 450 use weighted FVFs | Supported ordinary/weighted transforms; no FVF trimming was needed in these frames. |
| Pixel shader/register combiners | No active handles | No general implementation; narrow four-tap recognizer exists but was not hit. |
| Programmable vertices | None | Narrow admitted program/layout/opcode subset; not tested by these scenes. |
| Active texture operations | MODULATE, SELECTARG1, ADD, MULTIPLYADD, BLENDTEXTUREALPHA | Pattern-specific shader, not a general stage evaluator. Sixteen ADD and sixteen MULTIPLYADD draws bypass their requested stage-1 operation. |
| Stage-1 factor modulation | 349 draws request it without the host modulation flag; 152 have a nonidentity factor | The material-source admission gate omits some valid stage-1 operations; most nonidentity factors change alpha. Identity factors are harmless for this omission. |
| Environment reflection | 112 draws take the reflection host path; three additional reflection-like patterns at 6000 do not | Recognized patterns blend two textures. The additional FVF/layout pattern uses ordinary shading and loses that stage cascade. |
| DOT3 / EMBM / bump luminance | None in active stages | No general implementation; the sand bump lead remains unconfirmed. |
| Directional/point lighting | Both; all 96 point-lit draws in the sunset close-up attach lighting | Diffuse/ambient lighting is now supported on this branch. Spot/two-sided/specular support does not follow from that. |
| Fog | Enabled on 12,161 draws; linear and EXP2 modes with scene-specific parameters | Omitted entirely by the host shader. An enabled state establishes a support gap; per-pixel visibility loss depends on distance. |
| Specular | Enabled on 1,860 draws | No fixed-function specular term or specular vertex color consumption. Material/light specular contribution needs a follow-up capture before quantifying the image difference. |
| Stencil, depth, alpha and blending | Extensively used, including paired shadow draws and depth-gated composite | Implemented comparisons, masks, operations and factors; opaque MSAA cutouts can use host alpha-to-coverage. |
| Z bias | Nonzero on seven draws | Retained request is not forwarded to rasterizer depth bias. |
| Render-to-texture | 256x256, 512x256 and 256x512 color targets; all explicitly depthless | Host color production and later sampling succeed. No unsupported-target rejection or dropped shared depth was hit. |
| Textures | Stage-0 DXT1, DXT5, BGRA, P8 and linear BGRA | Every requested attached texture reached a host view. The 546 untextured draws are chiefly factor/fade/shadow paths, not upload failures. |
| Filtering | Active stages request linear min/mag and point/no mip; negative LOD bias also appears | Ordinary host sampler uses trilinear filtering and does not honor generic LOD bias. No trilinear-to-anisotropic upgrade was selected by these stage states. |
| Viewport / scissor | Guest viewports fill the current target | Host full-target viewport matches that layout after scaling. Subviewport/scissor behavior is untested. |
| Point sprites, cube/volume sampling, W buffer, nonsolid fill | No submitted use established; point-sprite enable is zero | Unsupported/retained-only paths remain untested. |

Fog mode names follow the public
[Direct3D fog enumeration](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dfogmode).
An active texture cascade ends at the first disabled color stage, per the
[stage evaluation contract](https://learn.microsoft.com/en-us/windows/win32/direct3d9/texture-blending-operations-and-arguments);
the counts above exclude stale later stages.

## Rejection and approximation inventory

The locations below name the relevant branch or function in the captured source
revision. **Reject** means no host draw, **fallback** means a draw can succeed with
changed shading, and **retain** means the setter stores state without implementing
its effect. A fatal API validation failure can stop before a complete frame can
be captured. Host allocation, shader compilation, buffer mapping, window closure
and command-queue failures are reported errors, not evidence of an artistic effect.

The last column separates a captured **requested-but-omitted** state from a
validation branch that never fired. Passing draw admission does not establish
faithful shading.

| Boundary and source location | Rejection, omission or approximation  Captured hit |
| --- | --- | --- |
| [Creation model](../../../recomp-runtime/d3d_creation_model.c#L12), [adapter](../../../recomp-runtime/d3d_creation_adapter.c#L211) | CreateDevice/Reset admit measured presentation modes only; unsupported parameters and allocation failures fail. The capture is downstream of successful creation. Observed successful creation; alternative modes/failures not hit. |
| [Frame model](../../../recomp-runtime/d3d_frame_model.c#L35) | Reject clear rectangles, unknown flags, invalid depth/stencil; any requested color-component clear becomes a whole-color clear. Swap admits only the modeled default flags and rejects counter overflow. No validation failure. Original clear component masks are not logged. |
| [Target adapter](../../../recomp-runtime/d3d_frame_adapter.c#L110) | Reject missing main/current color, undescribable offscreen color, non-renderable/depth color, and invalid custom depth. Aliasing wrappers of default depth are normalized. No rejection; small color targets and absent depth observed. |
| [Draw model](../../../recomp-runtime/d3d_draw_model.c#L176) | Only indexed triangle list/strip/fan and 16-bit indices. Missing stream/index storage, empty geometry and other topologies fail. No point, line, quad or patch host path. Point-sprite render state alone cannot create sprites. Only supported triangle strips; no rejection or other topology. |
| [FVF layout](../../../recomp-runtime/d3d_draw_model.c#L52) | Only XYZ, XYZRHW and one to three explicit blend weights; no indexed weights, XYZRHW normals or more than four UV sets. Host UVs are float2; declaration builder can express more than the draw consumer. Specular FVF bytes are stepped over but not shaded. Supported FVFs only; no indexed weights or extra UV sizes observed. |
| [FVF stream adaptation](../../../recomp-runtime/d3d_draw_model.c#L94), [indexed admission](../../../recomp-runtime/d3d_draw_adapter.c#L1611) | Trim trailing declared UV sets to fit stride; selected reflection layouts are remapped. Reject unsupported FVF, unmapped indices/vertices, missing/all-zero transforms and failed submission. This is not a general declaration interpreter. No remapping or rejection in these captures. |
| [Vertex blending](../../../recomp-runtime/d3d_draw_adapter.c#L324) | Require the matching retained blend mode and up to four world matrices. A disabled blend state uses WORLD0 even when weights are present. Weighted draws observed; no failed blend admission. |
| [UP admission](../../../recomp-runtime/d3d_draw_adapter.c#L2216) | Only four-vertex triangle strips in four measured FVF/stride combinations. Reject invalid positions/RHW, memory, target/filter/material admission or enqueue failure. One layout discards its disabled third UV set. Video/fade/logo/copy quads observed; no rejected UP draw. |
| [Pixel/material selection](../../../recomp-runtime/d3d_draw_adapter.c#L831), [host shader](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L115) | No general pixel-shader/register-combiner execution. Ordinary admitted FVF draws have no blanket active-pixel-shader rejection and can silently lose that effect. Only selected texture/factor/material-alpha/reflection/mask/filter patterns are implemented; arbitrary ADD, MULTIPLYADD, DOT3, signed/complement arguments and stage routing are not generally evaluated. Hit: ADD/MULTIPLYADD, omitted factor modulation and three missed reflection cascades. No active pixel shader. |
| [Four-tap filter](../../../recomp-runtime/d3d_draw_adapter.c#L581) | Exact active-combiner pattern only, four UVs aliasing one color texture, validated overrides and samplers. Reject mismatched constants/structure, texture storage/format/mips, coordinate selection or sampler state. Four saturated taps implement that filter, not general bump mapping. Not hit: no four-tap FVF or active pixel shader. |
| [Reflection admission](../../../recomp-runtime/d3d_draw_adapter.c#L708), [attachment](../../../recomp-runtime/d3d_draw_adapter.c#L731) | Require one fixed stage pattern, no active pixel shader and two 2D textures. Reject invalid/nonfinite transforms/material, unusable normal matrix, absent/linear/depth/palettized secondary texture. Coordinates are vertex reflection or mesh UV; no per-pixel bump perturbation, cube/volume mapping or general dependent fetch. 112 admitted reflection draws; three reflection-like layouts missed the outer selection gate. |
| [Alpha-mask attachment](../../../recomp-runtime/d3d_draw_adapter.c#L2131) | Exact two-stage UP pattern only; reject active pixel shader, wrong arguments/samplers or missing color/mask. Gaussian-cubic preview filtering is approximated by clamped linear sampling. Not hit: no separate alpha-mask host path or cubic-filter request. |
| [Lighting](../../../recomp-runtime/d3d_draw_adapter.c#L765) | Directional and point diffuse/ambient, at most eight lights. Skip attachment for programs, texture factor, pretransformed/no-normal/vertex-diffuse layouts, color-vertex/two-sided paths or unsupported stage selection. Spot lights, malformed/cyclic/over-limit lists, invalid material/light values or singular transforms abandon lighting attachment for the whole draw. No specular term. Directional/point attachment observed; spot/two-sided rejection not established. |
| [Material alpha](../../../recomp-runtime/d3d_draw_adapter.c#L505), [fallback shader](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L249) | Only measured material-source selection/modulation and exact zero-light case. Unattached lighting can fall back to texture-only shading, absolute normalized normal color or a constant tint; those are not original lighting. Point-light support on this branch supersedes the September survey's point-light gap. Material alpha/factor paths observed; actual unsupported-light fallback not established. |
| [Vertex-program admission](../../../recomp-runtime/d3d_draw_adapter.c#L1506) | Only one stream layout (position/normal/UV), bounded declaration/program payload and finite viewport conversion. Reject any active pixel shader, unrecognized stage/mask pattern, unsupported texture/sampler state, mip count or mask LOD bias. Viewport constants are rewritten. Not hit: no vertex program handle. |
| [Vertex translator](../../../recomp-runtime/d3d_vertex_program.cpp#L35) | Bound of 136 instructions; only three input attributes, 13 temporaries, 192 constants and selected outputs. MAC MOV/MUL/ADD/MAD/DP3/DP4 and ILU MOV/RCP/RCC/RSQ; other opcodes, relative addressing, bad final flags, illegal registers/outputs and constant writes reject. Fog output is not consumed by the pixel shader. Not hit: no vertex program reached translation. |
| [Texture attachment](../../../recomp-runtime/d3d_draw_adapter.c#L376), [palette](../../../recomp-runtime/d3d_draw_adapter.c#L415) | CPU upload supports linear BGRA, swizzled BGRA/A8/P8 and DXT1/3/5. P8 requires a valid 256-entry palette. Unmapped/unsupported data leaves texture attachment without upload bytes; ordinary host draws can continue without sampling. No general depth-texture, volume/cube or EMBM-format conversion. Supported texture/palette uploads observed; no missing requested host view. |
| [Texture lookup](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L2149) | Invalid dimensions/spans/pitch/palette, unsupported formats or GPU resource failures return no view. Specialized paths reject missing views; ordinary shading falls back. Compressed mips upload; uncompressed uploads expose level zero. Texture cache fingerprints sample content, so an unobserved rewrite can remain stale. No failed view or stale-cache event established; sampler/mip approximation applies. |
| [Offscreen color](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L1146) | Only linear/swizzled BGRA color targets; finite host dimensions, retained until backing storage release, shared 64 MiB target budget. Offscreen rendering stays at guest resolution and one sample. Offscreen BGRA targets observed; no budget/format failure. |
| [Depth target](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L1043) | Require matching guest color/depth sizes and D24S8 formats for custom depth. No depth binding when requested absent. A same-sized offscreen target sharing scaled/MSAA main depth deliberately draws without depth. Offscreen targets request no depth; no shared-depth drop or mismatch. |
| [Viewport binding](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L1254) | Host viewport always fills the bound target, depth range 0..1. Guest subviewports/scissor are not forwarded as general state. XYZRHW conversion reverses the host viewport; admitted programs have their own viewport-constant conversion. All guest viewports fill their target; subviewport/scissor gap not exercised. |
| [Draw submission](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L2405) | Reject invalid command layout/buffers/cull, unsupported pipeline, target failures, active offscreen color feedback, triangle fans over 4096 triangles, incompatible secondary-texture modes, missing specialized textures, unusable viewport/program scale and over-limit lights. Buffer/state creation and mapping can fail separately. No backend rejection; every captured draw returned success. |
| [Sampling](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L2111), [selection](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L2673) | Generic U/V modes are translated; unknown values wrap. No general border-color/LOD-bias/max-mip/aniso/texture-transform/color-key/alpha-kill implementation. Samplers use fixed defaults or recognized special paths; selected trilinear world textures are upgraded to 16x anisotropic. Failed sampler creation falls back to the default sampler. Hit: point/no-mip and negative-bias requests use default generic filtering. No cubic or anisotropic path. |
| [Depth/stencil decode](../../../recomp-runtime/d3d_render_state_model.c#L255), [blend decode](../../../recomp-runtime/d3d_render_state_model.c#L409) | Depth, alpha test, stencil comparisons/ops/masks, blend equations/factors/constant and channel masks are implemented. Unknown decoded enums retain model defaults; byte values are masked. USEW enables ordinary depth testing, not W-buffer interpolation. Shadow/depth/alpha/blend paths observed; no invalid enum or W-buffer request. |
| [Retained raster state](../../../recomp-runtime/d3d_render_state_model.c#L101), [shader interface](../../../recomp-runtime/d3d_presenter.h#L104) | Fill/edge-antialias/guest multisample state can be retained without changing host solid rasterization or configured MSAA. No general depth bias, polygon offset, logic op, clip planes, stipple, range/table/vertex fog, point size/scaling or point-sprite shader. Hit: fog and seven nonzero bias requests. Nonsolid/point-sprite paths not hit. |
| [Alpha coverage and dithering](../../../recomp-runtime/d3d_presenter_d3d11.cpp#L219) | Opaque main-target MSAA cutouts may use derivative-based alpha-to-coverage instead of hard byte clipping. Blended/offscreen draws keep hard tests. Main opaque RGB receives ordered dithering before UNORM quantization. These intentional enhancements alter original sample behavior. Configured MSAA coverage and main opaque dithering apply; no parity claim. |
| [Packet payload](../../../recomp-runtime/d3d_presenter_capture.cpp#L23) | Linear mutable BGRA is copied; other texture bytes remain borrowed until backend lookup. A cache miss after guest free/refill can observe later content. Per-span payload cap is 64 MiB. Packet copying exercised; no lifetime race established. |
| [Texture/tile/shader setters](../../../recomp-runtime/d3d_texture_adapter.c#L153), [tile](../../../recomp-runtime/d3d_tile_adapter.c#L18), [vertex binding](../../../recomp-runtime/d3d_vertex_shader_adapter.c#L36) | Invalid stage, tile/model or declaration binding can stop the runtime. Tile bookkeeping does not emulate GPU memory hardware. Nonintercepted driver work is not independently replayed by a push-buffer emulator. No setter/model failure; uncaptured GPU-only work remains outside coverage. |

## Highest-value next experiments

1. **Restore requested fog.** First add an opt-in fog-factor visualization for
   the captured linear title/sunset path and EXP2 transition path. Preserve the
   game's enabled flag, color and parameters; establish the original fog
   coordinate and units before choosing an eye-distance formula. Compare two
   camera distances and keep UI/disabled draws unchanged. This has the widest
   observed reach and should precede a new atmospheric effect.
2. **Fix the observed texture-stage omissions.** Start with stage-1 CURRENT ×
   TFACTOR independently of material-alpha admission, with a synthetic nonwhite
   alpha factor and one captured nonidentity case. Then implement exactly the
   observed ADD/MULTIPLYADD argument patterns and the missed reflection layout.
   Confirm their screen footprints with draw isolation. These are existing
   requested operations, not a speculative need for a general pixel compiler.
3. **Measure and restore specular, bias and sampler fidelity.** Capture material
   specular/power and light specular for one enabled character material before
   implementing its term. Test one nonzero-bias layer against a coplanar fixture.
   Compare requested point-mip/negative-bias sampling with the current trilinear
   default on a distant textured plane. Keep these separate experiments so the
   source of any visual change is attributable.
4. **Modernize the proven scene-copy blur and low-resolution reflections.** For
   the 256x256 close-up copy, first display the depth mask used by the existing
   full-screen composite; then compare a larger intermediate while retaining
   that mask and original blend. For rectangular scene targets, isolate a
   consuming shallow mesh and verify the reflected camera before increasing
   resolution. This can improve quality without replacing the whole pipeline.
5. **Preserve original shadow coverage before adding RTGI shadows.** Isolate one
   paired stencil/darkening sequence and retain its receiver mask. Compare an
   optional ray shadow against that footprint before replacing or combining it.
   Do not treat every stencil-enabled draw as a shadow. General combiner/bump
   work remains lower priority until a hotel/pool/shop/gallery capture actually
   exercises it.

## Coverage still needed

Use the private `PlayCapture.cmd` launcher and F11 for hotel, shop, casino,
pool, a player-controlled volleyball match and photo/gallery views. Capture both
an ordinary view and a transition/effect-heavy view; include text, particles,
previews, reflections and overlays. A photo/gallery preview and an in-game menu
are especially useful for the four-tap/alpha-mask paths. Take adjacent frames or
two camera angles where an overlay's role is ambiguous. The present directories
retain anonymous storage identities for correlation without exporting assets.

A screenshot plus state proves submission and resulting presentation, not the
identity of every overlapping polygon. Label sky, shadow, sand, particle and HUD
roles as candidates until an isolated-draw experiment confirms their footprint.
These agent runs are captures/smoke observations, not user gameplay acceptance.

## Validation

The authenticated Release runner built successfully and closed normally after
the capture sweep. Public-only Debug configuration/build and all 15 ctest tests
passed. The tools unittest run passed 19 tests with one optional real-extractor
check skipped; this includes authenticated manifest rejection checks. Public
export verification passes. The screenshot and call-order audit checked all
20 captures, with no adapter/backend rejects or truncated draw lists.
