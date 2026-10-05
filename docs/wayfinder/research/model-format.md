# Model geometry, draw palettes and scalar motion

This describes the supported readers, not a complete character format or a
validated animation replacement. The exporter produces a **primary-mesh bind
preview** using a separately supplied private rig. It can write an animation
from supplied solved local poses. It does not yet solve a motion bank into those
poses, reproduce a captured gameplay palette, or export secondary geometry.

## Geometry

Fields are little-endian. The model user record at file offset 20 starts with
`MDL`, followed by object count, texture count, another word and vertex-buffer
count. Object offsets at file offset 40 are file-relative. An object starts
with `OBJ`; its relevant fields are:

| Object offset | Meaning |
| --- | --- |
| 4 | Explicit float weight count, zero through three |
| 8 | Number of 32-byte vertex-layout blocks following offset 32 |
| 32, 36 | Vertex count and file-relative vertex-data offset |
| 40, 44 | Unsigned-16 index count and file-relative index-data offset |

The supported four-block layout has stride `32 + 4 * explicit_weights`:
float3 position, explicit float weights, float3 normal, float2 UV. Joint indices
are **not interleaved in vertices**. The final influence is `1 - sum(weights)`.
The exporter rejects negative or out-of-range influences instead of repairing
them. It normalizes nearly unit normals for glTF.

Material commands follow the layout blocks. The supported 144-byte form has
one texture stage, indicated at command offset 100. Its texture ordinal at
112 addresses the enclosing resource order. The geometry record at 128 carries
a declaration word, first index, strip triangle count and a trailing word.
The game submits `triangle_count + 2` indices as a triangle strip. The exporter
removes degenerate triangles and resolves textures through the existing ripper's
replacement key. Material lighting, transparency and deferred-pass behavior are
not reproduced by the preview material.

## Companion scripts and skinning

The five-word companion header has four relative section offsets. Some headers
encode them with arithmetic derived from the first word: initialize
`salt = ((word0 // 43) * 2537) // 67`; subtract it from each subsequent word
modulo 2^32, updating `salt = salt * 59 // 67` after each subtraction. This does
not compress the payload.

The first section has `(kind, relative_offset)` pairs and a zero terminator.
Kind 1 identifies another pair list containing pass identifiers and bytecode
offsets, also relative to the first section. Draw bytecode selects either a
single matrix slot or a palette recipe, and an object ordinal. Conditional
blocks select a variant. The reader reports skipped secondary/material opcodes
in the exported metadata. It does not claim that primary draws cover the whole
character.

The palette builder uses executable-owned recipes, not a joint-index stream
inside the mesh. A recipe starts with an output count. Each output is a sequence
ending in 255: a byte below 64 selects a matrix slot, bit 7 requests a positive
local offset, and bit 6 without bit 7 requests a negative local offset. Offset
indices use the low six bits. Selecting another matrix discards the current
composition. Translation acts in the selected matrix's basis. With row-vector
matrices this is `T(offset) * world`.

The draw consumes `explicit_weights + 1` matrices, in weight order. The bounded
recipe reader can parse only that consumed prefix. It rejects missing consumed
entries; it does not interpret bytes beyond a 16-byte recipe to manufacture an
unused output. The distinction matters when declared and consumed counts differ.

The game has a 32-slot matrix bank, including derived matrices. Its bind builder
walks an executable byte stream: ordinary values append an offset and save a
joint; 254 selects a previously saved joint as the new parent; 255 terminates.
Character-specific float triplets also come from executable tables. Archive
companion data does not establish a self-contained skeleton. No authoritative
bone-name strings have been recovered; retain numeric slot identities.

For a translation-only bind rig, all transforms of an exported draw must have
the same bind origin. Bake that origin into the vertices, use the selected
global bone indices as `JOINTS_0`, and store inverse global bind matrices in the
skin. The exporter checks the common-origin condition. This reconstructs the
palette offsets algebraically, but does not validate the live pose producer.
Derived slots still need independently validated bind and animation semantics.

## Scalar motion

A bank contains 1,000 clip offsets followed by a zero word. Offsets are relative
to the bank; slots may alias. Each clip starts with a sample count and a track
count, both unsigned 16-bit. The supported track count is 60, followed by 60
clip-relative offsets. Zero denotes an absent track, preserved as `None` by the
reader. It is not an implicit zero-valued joint rotation.

A segment's first unsigned-16 word contains its form in bits 0..1, duration in
bits 2..11, and value encoding in bits 12..15. Form 0 occupies ten bytes; form 1
occupies four. A zero-duration terminal occupies four bytes. Nonterminal forms
outside 0 and 1 are refused. Track reads stop at the next distinct track offset,
and clip reads stop at the next distinct clip offset.

The value is a custom 20-bit float. The header supplies sign and three exponent
bits; the next word supplies sixteen mantissa bits. For exponent zero, magnitude
is `mantissa / 65536`. Otherwise construct a float32 with exponent `e + 126`
and mantissa `m << 7`, preserving the sign. It is not IEEE half precision.

Form 0 also stores two 24-bit derivatives. Each uses a sixteen-bit word plus one
byte from the final word. Sign is bit 23, exponent bits 19..22, mantissa bits
0..18. Nonzero exponents become float32 exponent `e + 112`, with mantissa shifted
four places. Zero exponent and nonzero mantissa use another tier: float32
exponent `(m >> 16) + 105`, mantissa `(m & 65535) << 7`. All-zero magnitude is zero.

For endpoints `p0,p1`, duration `d` and endpoint derivatives `m0,m1`, form 0 is
cubic Hermite interpolation. With `s = (p1-p0)/d`, polynomial coefficients are
`p0`, `m0`, `(3*s-2*m0-m1)/d`, and `(m0+m1-2*s)/(d*d)`. Form 1 is linear. The
reader uses Python arithmetic; float32 coefficient rounding and game math
approximations have not been proven bit-identical.

Normal channel selection preserves archive track order. Mirroring permutes
limb/paired channels through a separate executable table and then transforms
the sampled pose. Pose storage begins with a three-word runtime prefix, so
scalar channel `k` lives at pose offset `12 + 4*k`.

| Channels | Observed consumer |
| --- | --- |
| 0..2 | Root displacement/height; root motion has gameplay effects |
| 3..5, 6..8 | Body Euler rotations |
| 9..11 | Head-chain Euler rotations |
| 12..19, 20..27, 28..35, 36..43 | Four limb groups: target position, endpoint orientation and solver controls |
| 44..46, 47..49 | Additional torso/neck Euler rotations |
| 50..59 | Paired corrections/additional channels; not all have direct main-clip keys |

Rotation values consumed by the trigonometric helpers are radians. The common
three-angle composition applies Z, then Y, then X operations to the current
row-vector matrix. Do not interpret sixty scalars as twenty independently
keyed joints. The final matrices include two-link IK, look-at, terrain correction,
transition layers and derived-joint blending. There is separate stateful
secondary motion.

Key spacing varies. At unit speed and one animation update per 60 Hz simulation
tick, the nominal scale is 60 clip indices per second. No bank field establishes
a universal frequency. Route cadence and speed factors must be measured for a
captured frame. The game evaluates integer samples and then performs
channel-specific fractional blending; direct fractional cubic evaluation is not
a compatible substitute. The public reader exposes integer samples only.

## Exporter input and output

`tools/model_export.py` uses the Python standard library and requires `--model`,
`--companion`, `--rig` and `--textures`. `--output` must name a new directory
beneath this checkout's ignored `private/`. Textures are existing key-named PNGs;
the glTF references them by relative URI. No source asset names are emitted.

The private rig JSON has:

- `joints`: entries with `parent` (-1 for a root) and a three-number local
  `translation`; array index is the source matrix slot.
- `offsets`: local float triplets used by palette operations.
- `palettes`: each entry is a list of `[bone_slot, [[offset_index, sign], ...]]`.
  Include exactly the entries consumed by its target draw.
- Optional `provenance`: explain the source and validation limits of this rig.

`--motion` and `--slot` decode one clip into `motion-scalars.json`. **That file
is diagnostic data, not an exported skeletal animation.** To write a glTF
animation, `--pose-samples` accepts already solved local poses: `times` contains
strictly increasing nonnegative seconds; `frames` contains one joint array per
time, each with `translation` and unit XYZW `rotation`. These must include IK
and derived-joint results, not raw archive channels. No pose solver is currently
provided. Exported interpolation is glTF LINEAR; it does not preserve the game's
unsampled fractional blend behavior.

The writer reflects Z for positions, normals and local transforms, reverses
triangle winding, and preserves source length units. It writes `model.gltf` and
`model.bin`. Follow the normative
[glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)
for buffer alignment, skin indices, matrix layout and animation samplers.
The built-in structural check checks the writer's buffer spans and mesh index
references; it is not a replacement for the full Khronos validator.

## Verification boundary and remaining work

Synthetic tests cover both scalar encodings, interpolation, absent/aliased
tracks, truncated records, mesh bounds, weight order, rotated palette offsets,
companion conditionals, inverse bind export and supplied-pose animation writing.
Public-only configure/build and runtime tests do not require retail inputs.
Private evidence and exact numeric hierarchy tables remain outside this document.

A complete animated character still requires a hand-written pose solver with
the correct limb tables, mirrored/masked blends, root policy, terrain/look-at
inputs and derived-joint operations. Capture the clip identity/cursor, actor
placement, rig identity and all procedural inputs together with the resulting
matrix bank and draw palette. Compare the independently reconstructed palette
at that same frame before accepting an animated export. A draw capture alone
does not supply this state, and a bind-pose preview is not that comparison.

Then add secondary geometry, remaining material commands and attachment paths.
Replacement needs a verified import/binding contract and a registered manual
runtime seam; this exporter does not load replacement models into the game.
