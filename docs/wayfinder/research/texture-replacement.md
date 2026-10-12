# Texture replacement and 3D foliage

Status: research for replacing low-resolution foliage art at 4K. It is based
on public source plus non-committed hands-on observation. No assets,
extracted names, or runtime captures were published.

## Decision

Start with hash-keyed texture replacement in the D3D11 presenter. Leave 3D
meshes for later. A replacement texture keeps the game's placement,
animation, draw state, and alpha test, so it improves the 4K image without new
scene knowledge. Swapping sprites for meshes needs per-instance world
positions that the presenter does not receive.

## Why foliage looks blocky at 4K

Jungle flowers and bushes are camera-facing quads with DXT1 textures of 64 to
256 texels. The game draws them with an alpha test that keeps a pixel when
`round(final_alpha * 255) > 192`. At 4K each texel covers many pixels, so the
texel grid and the cutout holes in the source art become visible. A native 1x
frame shows the same holes. MSAA alpha-to-coverage was tried and gave no
visible change, because SMAA already smooths the one-pixel edge. Only new
texture data changes the result.

## Replacement contract

- **Hook:** `lookupTexture()` in `recomp-runtime/d3d_presenter_d3d11.cpp`,
  after the backbuffer and render-target cases and before the cached or
  uploaded original is returned. Movies and other dynamic surfaces stay
  outside the first version.
- **Key:** format, original dimensions, mip count, and a full hash of every
  mip payload. Include the palette when P8 is supported. Exclude guest
  addresses, because the game reuses buffers. `textureFingerprint()` samples
  only 128 words; it detects cache changes and cannot identify a texture.
  `xbe/sha.cpp` already provides SHA-256. Queued compressed textures borrow
  guest memory (`d3d_presenter_capture.cpp`), so the capture packet must own
  every hashed mip payload before queuing, or compute and retain the full
  replacement key synchronously before queuing. Worker-side lookup must not
  hash borrowed bytes that the guest can refill for a later draw.
- **Fallback:** a missing, malformed, unsupported, or over-budget replacement
  uses the original upload. Cache both hits and misses; never touch the disk
  per draw.
- **Runtime format:** DDS with authored mips. BC7 needs feature level 11_0;
  the presenter also accepts 10_1 and 10_0, where the original is used.
  Keep UNORM sampling so the color pipeline does not change.
- **Dumping:** write each original DXT1 chain once, as DDS, before any
  substitution. Queued compressed textures borrow guest memory
  (`d3d_presenter_capture.cpp`), so a deferred dump must copy the bytes
  first.

## Custody

Dumps, hash lists, edited images, packs, and comparison captures are
extracted material. They stay under `private/` and are never committed, even
with hashed names. Public tests use synthetic textures. Packs are
user-supplied files loaded at runtime.

## Art guidance

Edit PNG masters and convert them offline with `texconv`. Process RGB and
alpha separately. Extend edge color into transparent texels to avoid dark
fringes. Rebuild every mip from the new art, and keep cutout coverage at the
192 threshold (`texconv --keep-coverage 0.7529411765`, or 192/255, is a
starting point). Do not let a driver generate mips for cutouts. AI upscalers cannot restore detail missing
from the source and can turn DXT block noise into invented structure. Small
silhouettes benefit most from hand work.

Complete mip chains, excluding allocation overhead:

| Size | BC1 | BC3 / BC7 | RGBA8 |
| --- | ---: | ---: | ---: |
| 512² | 0.17 MiB | 0.33 MiB | 1.33 MiB |
| 1024² | 0.67 MiB | 1.33 MiB | 5.33 MiB |
| 2048² | 2.67 MiB | 5.33 MiB | 21.33 MiB |

Choose the size from the on-screen footprint. The texture cache has 4096 FIFO
slots and no byte budget, so a large pack needs one.

## 3D foliage later

A texture hash identifies an image, not a plant. One draw can batch many
quads, and one atlas can serve several plants. The presenter draw command
carries a combined world-view-projection matrix, so it cannot recover a
plant's world pose. The draw adapter still sees the separate matrices before
composing them.

| Approach | Scope | Estimate |
| --- | --- | --- |
| Presenter-side substitution | One identified plant type in one scene | 1 to 2 weeks after identification, plus art |
| Game-side foliage cluster | Hand-written owners of instance identity, transforms, and visibility | 3 to 6+ weeks, uncertain |

Either approach must solve wind motion, LOD and culling bounds, lighting for
new normals, and shadow passes. The RTGI branch already sends world
transforms and alpha-tested geometry through the shared draw path. Resolve
any substitution before raster submission so rasterization and ray tracing
see the same geometry.

## Next gate

A DXT1-only pilot:

1. A dumped DDS loaded back unchanged renders identically.
2. A conspicuous synthetic replacement appears on the intended draws only.
3. The same texture at two addresses resolves to one key. Changed content at
   one address, including an unsampled word, resolves to a new key. Refilling
   a buffer after queuing preserves the earlier draw's replacement key.
4. A missing or malformed pack falls back to the original.
5. One reworked 2x or 4x foliage texture is compared in a bounded jungle
   scene at 1x and 4K, for distance, motion, cutout coverage, load count,
   frame time, and resident bytes.

## Sources

- [DDSTextureLoader](https://github.com/microsoft/DirectXTK/wiki/DDSTextureLoader)
- [Texconv](https://github.com/microsoft/DirectXTex/wiki/Texconv)
- [Block compression in Direct3D 11](https://learn.microsoft.com/en-us/windows/win32/direct3d11/texture-block-compression-in-direct3d-11)
- [Direct3D 11 on downlevel hardware](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-downlevel-intro)
- [Real-ESRGAN alpha handling](https://github.com/xinntao/Real-ESRGAN/blob/master/realesrgan/utils.py)
- [PCSX2 texture replacements](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/Renderers/HW/GSTextureReplacements.cpp)
