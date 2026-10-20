# Thin net shadows: filtering and ownership options

Research date: 2026-10-15. Source inspection only, against the working tree at
`6a4d6f8` plus existing uncommitted changes. No runtime edits, builds, runners,
or gameplay validation. This describes the research baseline before the subsequent caster-exclusion fix.
External references establish rendering methods, not
the contents or ownership of this game's shadow overlays.

**Recommendation:** establish exclusive legacy/sun ownership first; then test
alpha coverage and shadow-map sampling separately. Increasing PCF width alone
cannot recover wires discarded while building the map.

The inspected [shadow implementation](../../../recomp-runtime/d3d_sun_shadow.h)
uses a single-sample 4096-square map, base-mip caster alpha, and 25 comparison
samples with footprint-dependent spacing. It excludes the net/post assembly
through a bounded geometry/material rule. The
[presenter](../../../recomp-runtime/d3d_presenter_d3d11.cpp) suppresses recognized
legacy overlays' color writes only when the sun map is valid, preserving their
depth/stencil behavior. These are source properties, not proof of which objects
share an overlay; see also the [feature notes](../../sun-shadow.md).

| Mechanism | What it changes | What it cannot establish |
| --- | --- | --- |
| Spatial PCF | Averages **depth-comparison results** around a receiver in the current map. Wider support smooths visibility but broadens shadows. [GPU Gems, chapter 11](https://developer.nvidia.com/gpugems/gpugems/part-ii-lighting-and-shadows/chapter-11-shadow-map-antialiasing) | Cannot reconstruct caster coverage absent from that map; filtering raw depths is not PCF. |
| Stable projection | Fixed scale and texel-aligned light-space translation reduce camera-induced resampling. [Microsoft shadow-map guidance](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/common-techniques-to-improve-shadow-depth-maps#moving-the-light-in-texel-sized-increments) | Uses no history. Does not resolve subtexel wires, changing light direction, or receiver minification by itself. |
| Alpha-mask filtering | Filters the caster's texture footprint before deciding coverage. Coverage-preserving mips rescale alpha for a specified cutoff. [Microsoft DirectXTex](https://github.com/Microsoft/DirectXTex/wiki/ScaleMipMapsAlphaForCoverage) | Preserves approximate occupied area, not exact wire topology. Forced mip zero avoids mip dilution but leaves minification undersampled. |
| Multisample coverage | Alpha-to-coverage converts shader alpha into a sample mask; conversion can vary by hardware. [Microsoft blending documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-blend-state#alpha-to-coverage) | Scene MSAA does not add samples to a separate single-sample shadow map. Binary discard before conversion still loses rejected coverage. |
| Temporal filtering | Accumulates samples over frames and validates history. [Yang, Liu and Salvi, 2020](https://research.nvidia.com/labs/rtr/publication/yang2020survey/) | PCF, texel snapping, and a fixed noise pattern are not temporal accumulation. Repeated identical missing samples supply no new information. |

## Ownership before filtering

The following is a proposed integration rule, not a claim about the game's
mechanism: assign each light/caster/receiver contribution to one representation.
For a black legacy overlay with alpha `a`, ordinary alpha blending leaves
`C * (1-a)`. Applying an additional sun attenuation `s` produces
`C * (1-a) * s`; duplicating the same blocker therefore darkens it twice.
This follows from [Microsoft's blend equation](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-blend-state).

- **Legacy-owned group:** retain its overlay and exclude every corresponding
  caster contribution from the sun result. Excluding the net alone is
  insufficient if that overlay also contains posts or trees.
- **Sun-owned group:** admit its replacement casters and suppress its verified
  legacy color contribution only where replacement coverage is available.
  Preserve depth/stencil effects and restore legacy rendering when unavailable.
- **Mixed overlay:** split only with evidence of separable draws, producers,
  or masks. Otherwise choose ownership for the entire group or defer that
  replacement. Similar texture dimensions or a world-space box do not prove
  semantic ownership. Avoid broadly re-enabling scenery decals as a net fix.

A valid map is not proof of replacement at every receiver: check its bounds,
edge fade, camera interval, and admitted casters. A fallback transition needs
complementary weights on the **same group's visibility**, composed once;
multiplying two partially faded darkening passes is not equivalent. Already
composited legacy color may need a separate mask to make that possible.
Likewise, multiplying averaged visibility from separate maps is generally an
approximation for overlapping blockers; evaluate joint visibility at common
samples where exact overlap matters. These are design constraints, not a
proposal to add a general shadow compositor now.

## Ranked, bounded experiments

1. **Keep a narrowly verified legacy group temporarily.** Lowest implementation
   cost if its full ownership is known. Retain only that group and exclude its
   matching sun casters. Reject if it restores unwanted scenery, gives duplicate
   silhouettes, or requires unproven object classification. This cannot modernize
   a legacy post shadow inseparable from the net shadow.

2. **Test coverage-preserving alpha mips in the existing map.** Limit the trial
   to one verified cutout material; use its shadow-pass texture footprint and
   actual cutoff. DirectXTex documents a reference method that rescales mip
   alpha after mip generation; it does not directly process compressed inputs.
   This is an algorithm reference, not a dependency recommendation.
   [Microsoft](https://github.com/Microsoft/DirectXTex/wiki/ScaleMipMapsAlphaForCoverage)
   Keep PCF and bias fixed initially. Reject if area is retained only by turning
   the mesh into a broad opaque band. At extreme minification, fractional
   coverage is a better target than individually resolved wires.

3. **Increase sampling only for the net region.** Trial a tightly fitted,
   fixed-scale, texel-aligned light projection or bounded supersampling; include
   the receiver footprint and off-screen casters. Microsoft's guidance supports
   tighter allocation and stable projection, with clipping and bias tradeoffs.
   [Microsoft](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/common-techniques-to-improve-shadow-depth-maps)
   A dedicated map adds allocation, pass, and overlap-composition costs. Reject
   if improvement needs an unbounded global resolution increase or breaks
   coverage at the region's edge.

4. **Try hashed alpha coverage if deterministic cutoffs still lose wires.**
   Wyman and McGuire replace a fixed cutoff with a spatially hashed threshold,
   retaining distant alpha geometry at the cost of noise. Their method reduces
   flicker without history accumulation.
   [Paper and abstract](https://research.nvidia.com/labs/rtr/publication/wyman2017hashed/)
   Applying it to this caster pass is an experiment: use stable object-space
   inputs and scale-aware sampling, not a new frame-random threshold. Reject
   visible grain, swimming, or incorrect average shadow density.

5. **Consider shadow-pass alpha-to-coverage only with a sampling redesign.**
   A small multisampled trial can retain fractional coverage, but requires
   shader alpha output and multisampled shadow storage. Read and compare
   individual depth samples, then average visibility; averaging depth first
   does not preserve visibility. D3D11 exposes sample-indexed
   [Texture2DMS loads](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-object-texture2dms-load).
   This is more than enabling the presenter's existing MSAA. Reject excessive
   bandwidth, vendor-dependent grain, or unchanged subpixel loss.

6. **Defer temporal accumulation until spatial coverage is sound.** A bounded
   shadow-visibility history trial would need reprojection, history rejection,
   and resets on camera cuts, light/owner changes, and invalid maps. Test moving
   casters and newly exposed receivers for lag and ghosts. The cited
   [TAA survey](https://research.nvidia.com/labs/rtr/publication/yang2020survey/)
   identifies accumulation and history validation as separate essentials;
   this integration is substantially larger than changing PCF taps.

## Validation criteria for the implementation owner

Use independent synthetic masks and geometry first; change one mechanism at a
time. Keep each runtime observation under one minute; this note executes none.

- Sweep a thin grid through subtexel translations, scale, and rotation. Compare
  mean shadow coverage and wire contrast against a supersampled reference;
  inspect motion, not only still images. Reject popping and apparent stability
  obtained solely by excessive blur or darkening.
- Separate caster-mask loss from receiver-filter loss by inspecting map coverage
  and final visibility independently. Include sloped receivers and low-angle
  light; reject acne, detached shadows, and clipping.
- Compare legacy-only, sun-only, and mixed ownership with isolated and overlapping
  synthetic blockers. Mixed ownership must add neither a second silhouette nor
  extra same-blocker attenuation; unrelated trees/posts must retain their chosen
  owner. Exercise map borders, invalid maps, and depth/stencil side effects.
- Measure GPU cost and storage against the existing path. Check scene MSAA off/on
  independently of shadow sampling, then request a named-build user play test.
  No option here establishes the game's exact overlay composition or acceptance.
