#ifndef DOAXBV_RECOMP_BODY_MORPH_H
#define DOAXBV_RECOMP_BODY_MORPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How DOAXBV blends a girl's morphing parts (hands, face, chest) between
   stored shapes, as plain maths on host data (docs/body-morph.md). Nothing
   here knows the guest: body_morph_adapter.c feeds it the game's records.

   A blend writes `count` vectors (three floats) into a vertex buffer, the
   n-th at byte offset offsets[n], from shapes of `count` packed vectors:
     one    the shape itself                     0x00065890
     two    wa * a + wb * b                       0x00065910
     three  (wa * a + wb * b) + wc * c            0x000659F0
   each summed left to right in double and rounded once to float, as the
   game's x87 code does. The mirrored kind (0x000658D0, 0x00065980,
   0x00065A80) negates x: a part on one side reuses the other side's shapes.

   `buffer` holds `size` bytes; a write past it is skipped. */
typedef enum RecompMorphKind {
    RECOMP_MORPH_ONE,
    RECOMP_MORPH_TWO,
    RECOMP_MORPH_THREE,
} RecompMorphKind;

/* Which blend a record asks for: one shape when its two shapes are the same
   and the third weight is zero, two when they differ and the third weight
   is zero, otherwise three (a NaN weight counts as non-zero, as the game's
   compare does). */
RecompMorphKind recomp_morph_kind(uint32_t shape_a, uint32_t shape_b, float third_weight);

void recomp_morph_blend(uint8_t *buffer, size_t size, uint32_t count, const uint32_t *offsets,
    RecompMorphKind kind, bool mirrored,
    const float *a, float wa, const float *b, float wb, const float *c, float wc);

#ifdef __cplusplus
}
#endif

#endif
