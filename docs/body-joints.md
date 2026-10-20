# Body joints

How the game bends the girls' joint pieces each frame, and the readable
source that now runs it. Every number below is the game's; the readable code
was checked against the generated code call by call (see **Checking**).

## The pieces

A girl's body is rigid pieces, each fixed to one bone, plus a joint piece at
every place two of them meet: shoulders, elbows, wrists, hips, knees and
ankles. Her body model lists the joints; when she is loaded
(`0x000672B0`..`0x0006749A`) each becomes a 32-byte record at
`0x4CA640 + n * 0x400` (up to 32; the count is the byte at `0x4CB644 + n`):

| Offset | Holds |
|---|---|
| `+0x00` | the joint piece's vertex buffer |
| `+0x04` | its interior list |
| `+0x08` | its frame data (0xA0 bytes, then the interior vertices) |
| `+0x0C`, `+0x0D` | bones a and b |
| `+0x0E` | skip flags: 1 or 2 for joints on a foot (bone 3 or 9; 2 when the model marks it, bit 0x08), 4 for joints on the toes (bone 18 or 22) |
| `+0x10`, `+0x14` | the buffers the two lists below point into |
| `+0x18` | the copy list |
| `+0x1C` | the transform list |

The model stores the lists as offsets; the load turns them into pointers
(`0x000674A0`, `0x000674F0`) and marks the joint done (bit `0x80` of its
flags byte in the model).

## The bend

The draw task (`0x000B42B0`, through `0x000B4450` and `0x00063051`) calls
`0x00067540` for each girl it draws, with her two bone matrix tables (her
model's `+0x1C` for bone b, `+0x20` for bone a). It skips the records whose
flags meet her mode's mask (mode byte `0x4C9744 + n`: 1 skips 6, 2 skips 5,
otherwise 2; kept at `0x4CB640 + n`). For each other record the current
matrix (`0xA24190`) becomes bone b's matrix times bone a's
(`0x00186FB5`), and `0x00067630` bends the piece in four steps:

1. **Copy list.** Vertices that sit exactly on a neighbouring piece take its
   position, and its normal unless the source's low bit is set. The list is
   groups of a source and its targets, each group ended by -1, the list by
   a -1 where the next source would be.
2. **Transform list.** The same groups; the source (a vertex of the other
   bone's piece) goes through the current matrix, position and normal (the
   normal 0x18 bytes in), into every target.
3. **Frames.** From the first 0xA0 bytes of the frame data: three axes,
   each the parent's (as stored), the child's (through the current matrix)
   and their normalised sum, the halfway axis; and three origins, the
   parent's, the child's and the midpoint of the child's and a stored
   point. They land in the scratch at `0x4CA500` (four matrices).
4. **Interior.** The interior list is a step k, then groups of targets. For
   k below 16 the step's frame becomes the current matrix: each axis and
   the origin blended from the parent's, the child's and the halfway one
   with the weights at `0x333770 + k * 16`, then made orthonormal (the third
   axis normalised, the second made square to it and normalised, the first
   square to both and normalised). Each group takes the next 32-byte vertex
   after the frame data (position, normal) through that frame. A step of
   16 or more keeps the last frame; a step of 0 ends the list.

The weights are a quadratic through the parent frame (k = 0), the halfway
frame (k = 8) and the child frame (k = 16), at t = k / 16:
parent (1 - t)(1 - 2t), child t(2t - 1), halfway 4t(1 - t). So a joint's
inner rings follow the bend smoothly from one bone to the other.

Lengths use the game's square root: a reciprocal square root refined by one
Newton step in float, `x * (1.5 r - 0.5 x r^3)`. A length of zero leaves the
vector as it is; so does a zero projection when making axes square.

## Readable source

- `recomp-runtime/body_joints.c`: the maths, on plain arrays, with nothing
  of the guest: the matrix product, rotate, transform, place, the square
  root, the frames, the weights and the blended frame. Another game's
  models can use it as they are.
- `recomp-runtime/body_joints_adapter.c`: the two game functions over the
  guest's records and scratch.
- `recomp-runtime/body_joints_test.c`: the weights against the game's
  table, and the helpers.

**Seam.** The bend is registered for `0x00067540` in the manual lookup
(`recomp_body_joints_lookup_manual`); the recipe lists it as a manual call
target so its direct call reaches it. It calls no game code. The record
builder (`0x000672B0`) is still the game's.

## Checking

`RECOMP_BODY_JOINTS_VERIFY=1` runs each call twice from the same state:
first the generated code, then the readable code. It compares the current
matrix, the scratch (`0x4CA4E0`..`0x4CA63F`), the mask byte and every vertex
the girl's lists write. Six words are not compared: the w lanes of the two
axes made square to others (`0x4CA60C`, `0x4CA61C`), of the current matrix's
copies of them (`0xA2419C`, `0xA241AC`) and of the placed vertex
(`0x4CA4EC`, `0x4CA4FC`). The game's helper (`0x0017B080`) subtracts a
stack word it never wrote there, and no vertex reads those lanes.
