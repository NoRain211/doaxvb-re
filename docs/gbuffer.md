# Optional D3D11 normal and depth buffers

Set `RECOMP_D3D_GBUFFER=1` before starting the runner. This adds a normal MRT
to the visible main scene and resolves normals and depth once at present.
Unset, `0`, or other values keep the original draw shaders, one color target,
and depth allocation (unless RTGI independently requests readable depth).
No G-buffer textures, shader compilation, clears, or resolve passes run off.
This uses ordinary D3D11 resources; no vendor features or RTGI build are needed.
Creation failure releases partial G-buffer resources and disables the feature.

`RECOMP_D3D_GBUFFER_VIEW=normal` shows encoded world normals as RGB, with
black for missing normals. `depth` shows near pixels brighter using
`log2(1 + 1024 * (1-z)) / log2(1025)`; cleared depth is black. These views
require the feature to be enabled, fill the presentation output, and bypass
gamma and SMAA. Ordinary color rendering retains those effects.
In game, F11 cycles the game image, normal view and depth view.

Normals use `R10G10B10A2_UNORM`: RGB = `normal * 0.5 + 0.5`, A = 1 for a
valid unit vector, or all zero for missing/degenerate normals. Consumers decode
valid pixels with `normalize(RGB * 2 - 1)`. The draw shader
interpolates vertex normals transformed by the existing inverse-transpose
world matrices, including the visible draw's skin weights, then normalizes
per pixel. Only host-lit, normal-bearing fixed-function draws with depth
test/write and color writes enabled write normals. Target 1 never blends;
the original alpha test, sample coverage, depth and stencil tests apply.
Unlit decals, screen-space UI, vertex programs, offscreen/custom-depth draws,
and the sun caster pass leave it untouched. Main color/depth clears reset it.

The main depth texture becomes `R24G8_TYPELESS`, retaining its
`D24_UNORM_S8_UINT` DSV and stencil behavior, plus an `R24_UNORM_X8_TYPELESS`
SRV shared with RTGI. See Microsoft's [depth resource/view example](https://learn.microsoft.com/en-us/windows/uwp/gaming/create-depth-buffer-resource--view--and-sampler-state)
and [per-target write masks](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_render_target_blend_desc).
The resolve unbinds scene outputs, selects the minimum device-depth sample,
and copies that sample's normal instead of averaging across edges. This works
at internal resolution with 1x and 8x MSAA. Presenter-local consumers can use
`gbuffer.resolved[0]` (packed normal) and `[1]` (`R32_FLOAT` device depth),
both single-sample SRVs valid until the next resolve or presenter destruction.

At 2880x2160, excluding driver padding/compression:

| Allocation | 8x MSAA MiB |
| --- | ---: |
| Added multisample normal | 189.84 |
| Added resolved normal | 23.73 |
| Added resolved depth | 23.73 |
| **Added total** | **237.30** |
| Existing scene depth, unchanged size | 189.84 |

At 3840x2160 the added total is 316.41 MiB. Scale follows the presenter's
aspect rules; scale 4.5 with widescreen off gives 2880x2160.

Depth is raw nonlinear device Z, not camera distance. A later consumer needs
the matching camera inverse projection/view to reconstruct positions. Unlit
depth-writing geometry can cover retained normals from an older surface;
alpha is normal validity, not a guarantee that every depth writer supplied one.
There are no texture/detail normals or vertex-program normal reconstruction.
No lighting/shadow consumer is added. Future work can use these views for
normal-offset sun-shadow bias or per-pixel character N dot L after defining
camera/depth-interval ownership and the treatment of uncovered materials.

The presenter test reads back scaled 1x/8x normal/depth pairs, unlit
preservation, alpha/depth rejection, independent blending, nearest-sample
selection, debug output, clears, failure cleanup and resource release. It also
compiles the FVF shaders with sun shadows and per-pixel lighting on/off.
