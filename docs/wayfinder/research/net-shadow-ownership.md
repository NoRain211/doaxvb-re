# Net/post fallback and shadow ownership

## Fix following the investigation

The caster prepass did not consult the recognized legacy-overlay identity.
A terrain-following palm decal can satisfy the scenery height/width test and
cast its baked silhouette into the new map even when its visible color pass
is suppressed. The texture cache now distinguishes palm from net/post overlays;
both are excluded from casting, while only palms lose visible color writes.
The presenter regression drives the actual caster entry point with a tagged
warped decal and an otherwise identical untagged control.

The fixed material/position rule for physical net/post geometry remains a
bounded shortcut; its transform-equivalence limitation below still applies.

## Earlier research findings

The mixed fallback does not yet establish one shadow system per object.
The original combined net/post overlay is enabled by removing its three
payloads from `sunStaticOverlay`. Independent palm overlay identities and
live palm target tracking remain enabled. Restoring a combined overlay cannot
select just its net pixels or remove foliage drawn into its post silhouette.

Source: [shadow classifiers](../../../recomp-runtime/d3d_sun_shadow.h),
`sunStaticOverlay`, `sunPalmTexture`, `sunPalmProducer`, `SunPalmTarget`;
[texture lookup and draw submission](../../../recomp-runtime/d3d_presenter_d3d11.cpp),
`lookupTexture`, `submitDraw`.

## Confirmed implementation limits

- `sunNetPost` uses texture format/dimensions, an identity world matrix and a
  fixed world box. It does not identify the owning net/post object. A translated
  local representation of identical world geometry fails the identity guard.
  Removing that guard alone would broaden false matches with moving objects.
- Before the fix, the receiver and caster classifiers were independent. Recognizing a legacy
  overlay does not prove that all corresponding sun casters are excluded.
  Excluding a caster does not prove that a matching legacy overlay is present.
- `prop_shadows` counts suppressed overlay draws. It neither counts unmatched
  legacy silhouettes nor identifies which objects cast into the sun map.
- Static palm textures have a bounded allowlist. Live palm classification also
  requires particular dimensions, producer state and target lifetime. An
  unmatched palm path can retain its old shadow, but that is not evidence that
  this fallback changed palm suppression.

Sources: `sunNetPost`, `sunSceneDraw`, `sunStaticOverlay`, `sunPalmTexture`
in the shadow header; `d3d11_backend_shadow_command`,
`d3d11_backend_shadow_end`, `submitDraw` and `lookupTexture` in the presenter.

## What remains unproven

The user reports both standalone palm shadows and foliage incorporated in the
post silhouettes. The combined net/post overlay explains the latter only.
The standalone-palm report requires its own contributing-draw investigation;
unchanged palm suppression code is not evidence that it covers every palm path.
The report does not establish which net/post component escapes the caster
exclusion in the reported scene.
The transform counterexample is a classifier weakness, not proof that this
transform occurs on the reported frame. Existing presenter tests are not a
cross-court object-ownership test.

Source: [presenter tests](../../../recomp-runtime/d3d_presenter_d3d11_test.cpp),
prop suppression tests and net/post selection checks. Local extracted images,
logs and analysis are kept outside the public documentation tree.

## Recommended order

1. Identify one reported affected frame and replay the same packet with only
   one shadow family changed: combined net/post overlay, static palm overlay,
   live palm overlay, then net/post sun casters. Preserve depth/stencil writes
   in every color suppression control. Record the contributing draw identities,
   not just aggregate counters.
2. Trace the owning static court mesh/material group at the adapter boundary.
   Find a stable group identity that survives camera movement and object
   transforms. Do not classify an object from texture dimensions, draw order,
   a guest allocation address, or shared mesh-drawing helper alone.
3. Assign legacy shadow ownership to the entire net/post assembly, including
   attached foliage and bindings. Suppress its sun casters using the same
   ownership decision. Retain the independent palm and character/ball paths.
   Activate this fallback only where the corresponding legacy overlay exists.
4. Test both court ends, each court and time of day, camera pans, offscreen
   transitions, a ball/player crossing the net and the hotel. Verify original
   shadows with sun disabled. Compare moving footage as well as still captures.

If the desired result is sun shadows for the posts and a legacy shadow only
for the net, the combined legacy silhouette needs to be separated first.
Masking the post regions of that overlay is an asset-specific alternative;
it requires inspection and validation for every supported texture variant.
It is not achieved by changing a whole-draw texture allowlist.

## Separate sampling problem

The sun pass has no temporal history: it clears/rebuilds a depth map and uses
spatial comparison filtering. Temporal-looking instability therefore does not
by itself identify a temporal accumulation bug. The current 4096-wide map over
64 world units gives 0.015625 units per texel before projection. Thin geometry
and binary alpha coverage can change before receiver-side filtering samples
that map. More PCF taps cannot guarantee that missing caster coverage returns.

Sources: `sunCreate`, `sunFit`, `sunShaderSource` in the shadow header and the
caster pass in the presenter; Microsoft's
[shadow depth map techniques](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/common-techniques-to-improve-shadow-depth-maps)
distinguish projection aliasing, resolution, bias and stabilization. See also
[filtering options](net-shadow-filtering-options.md). Fix ownership before
another filtering change so double shadows do not contaminate the comparison.

## Acceptance gate

A diagnostic must show exactly one contributing shadow path for each net/post
component, independent palm sun shadows, no inherited palm overlay when its
replacement is valid, and unchanged opt-out rendering. A same-frame ablation
must account for the reported silhouette. A camera-motion sequence must test
stability; a clean still frame and a passing synthetic predicate test are
insufficient.
