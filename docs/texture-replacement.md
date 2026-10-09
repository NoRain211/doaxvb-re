# Texture replacement (D3D11)

Start the runner with `RECOMP_TEXTURES=1` to use `private/textures/replace`,
or set `RECOMP_TEXTURES` to a directory under the working directory's
`private/` tree. Absolute and relative paths must stay inside that tree.
Set `RECOMP_TEXTURE_DUMP=1` to also write
each distinct original texture to `private/textures/dump/<key>.png`.
`RECOMP_TEXTURE_DUMP_DIR` can choose another folder inside the same `private/` tree.
Paths that resolve outside it disable this feature with a diagnostic.

1. With either option enabled, press **F12** in the game. Originals used by
   the current frame go to `private/textures/frame-<present>/`.
2. Find the image in that folder, copy it into the replacement directory,
   and edit it in an image editor. Keep its filename and alpha channel.
3. Save as PNG, return to the game, and press **Ctrl+F12**. Changed and new
   files load when their textures are next used. Remove a file and reload
   to restore the original.

Keys contain the original format, dimensions, mip count, and SHA-256 of
every mip payload plus the P8 palette. Guest addresses are excluded, so
identical textures share a replacement, including on different objects.
Changing one shared texture changes every draw that uses that content.
All dumps, edited images, packs, hash lists, and captures belong under
`private/`; do not commit them.

Static DXT1, DXT3, DXT5, ARGB32, P8 and A8 textures are supported. Movie
buffers, depth surfaces, backbuffers and rendered targets are excluded.
PNG replacements may change resolution but must keep the original aspect
ratio. Sampling stays UNORM with straight alpha; draw alpha tests and
blending are unchanged. No color-profile conversion is applied.

Edited PNGs get GPU-generated mips. An unchanged dump keeps the original
GPU texture and authored mips, preserving exact sampling. Generated mips
can change thin cutouts at a distance. For control over every mip, supply
`<key>.dds` with authored mips: BC1, BC2, BC3 or RGBA8, using legacy DDS or
DX10 headers. BC7 requires feature level 11.0. Arrays, cubes, volumes,
premultiplied alpha and sRGB DDS formats are unsupported. PNG takes priority
when both extensions exist.

File scans, loads and readbacks run at frame boundaries. Drawing uses
cached hits and misses without filesystem access. First-use loads and
dumps can pause a frame; turn off continuous dumping for normal play.
Reload detects file timestamp, size or extension changes. Save normally
rather than restoring an older file timestamp.

Missing, malformed, incompatible or over-budget files use the original.
The log prints one loaded/rejected line per attempted file version.
Limits are 64 MiB per file or decoded PNG, 512 MiB of replacement mip
payloads, 128 MiB of original identity snapshots, 16,384 cached keys/files,
and 4,096 distinct textures per frame. Hardware allocation limits can
reject a texture earlier. All supported formats except BC7 work at D3D
feature level 10.0.

For repeatable local captures, `RECOMP_TEXTURE_FRAME_AT=<present>` requests
one frame's texture dump at that present number. It still requires one of
the two enable options.

The synthetic tests cover full-content identity, palette and mip changes,
ownership of queued bytes, PNG round trips, changed-file reload, fallback,
authored DDS mips, and PNG mip generation on a feature-level-10.0 device.
Relevant API contracts: [WIC pixel formats](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-codec-native-pixel-formats)
and [D3D11 mip generation](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-generatemips).
