# Body morph

How the game blends a girl's morphing parts between stored shapes (her
hands' gestures, her face, her chest and the swimsuit pieces over it), and
the readable source that now runs the blend. Every number below is the
game's; the readable code was checked against the generated code call by
call (see **Checking**).

## The shapes

A girl's hands, face and chest are single meshes whose vertices the game
rewrites each frame from stored shapes. Her body model lists them: the
entry after her geometry entry (both in `datadvd.afs`) holds, at the offset
in its header word `+0x0C`, a morph section:

| Offset | Holds |
|---|---|
| `+0x00` | `apv\0` |
| `+0x04` | the number of morph sets |
| `+0x08` | each set's offset from the section, one word a set |

and each morph set (offsets from the section; pointers once loaded):

| Offset | Holds |
|---|---|
| `+0x00` | the number of shapes |
| `+0x04` | the mesh (its OBJ index in the geometry entry); bit 31 set: the shapes are the other side's, mirrored in x |
| `+0x08` | the positions it writes |
| `+0x0C` | the normals it writes (0 for the hands) |
| `+0x10` | its shapes, 8 bytes a shape: positions, normals (three floats a vertex) |
| `+0x14` | the positions' byte offsets in the mesh's vertices (32 bytes a vertex) |
| `+0x18` | the normals' |

Each hand has 31 to 35 shapes; shape 0 is the mesh as stored, shape 24 a
closed fist and shape 14 a flat open hand, the same for every girl.

## The blend

Each girl has blend records (0x24 bytes) for her face, hands, chest and
swimsuit pieces, which `0x00064300` visits in turn, calling `0x00064FD0`
with the record in ESI:

| Offset | Holds |
|---|---|
| `+0x00` | the morph set |
| `+0x04` | the model |
| `+0x08`, `+0x0C`, `+0x10` | shapes A, B and C |
| `+0x14`, `+0x18`, `+0x1C` | their weights |
| `+0x20` | the model's vertex buffer number |
| `+0x21` | the kind: 1 mirrored in x |
| `+0x22` | dirty |

A record that is not dirty is left alone. A dirty one is cleared and, with
a model and a set, the model's vertex buffer is locked (`0x001E4D00`; the
buffer is kept at `0x004C9C38`) and one of three blends written into it,
the positions and then the normals:

- **one** shape (A and B the same, weight C zero): A itself (`0x00065890`);
- **two** (A and B differ, weight C zero): `wa * A + wb * B` (`0x00065910`);
- **three** (weight C not zero): `(wa * A + wb * B) + wc * C` (`0x000659F0`).

Sums are in double, rounded once to float, as the game's x87 code does. The
mirrored kind of each (`0x000658D0`, `0x00065980`, `0x00065A80`) negates x.
The blend comes from the table at `0x0033F0D0`, two entries a blend.

## Readable source

- `recomp-runtime/body_morph.c`: the blends on plain arrays, with nothing of
  the guest.
- `recomp-runtime/body_morph_adapter.c`: `0x00064FD0` over the game's
  records.
- `recomp-runtime/body_morph_test.c`: which blend a record asks for, the
  sums, the mirrored kind and bit-exact copies.

**Seam.** The blend is registered for `0x00064FD0` in the manual lookup
(`recomp_body_morph_lookup_manual`); the recipe lists it as a manual call
target so the direct calls from `0x00064300` reach it. The D3D vertex
buffer lock (`0x001E4D00`) is library code and is called as it is. A table
entry other than the six known blends runs the generated function.

## Checking

`RECOMP_BODY_MORPH_VERIFY=1` runs each call twice from the same state:
first the generated code, then the readable code. It compares the record,
the locked buffer word, the lock's own result (the same resource locked
and the same buffer returned) and every vertex the blend writes. The push
buffer the lock writes to is not compared: the second lock of a check can
skip the fence kick the first one made.
