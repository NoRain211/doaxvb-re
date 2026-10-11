#ifndef DOAXBV_RECOMP_UNLOCK_SWIMSUITS_H
#define DOAXBV_RECOMP_UNLOCK_SWIMSUITS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The size of the game's save image; the save code hashes it in one piece. */
enum {
    RECOMP_SAVE_STATE_SIZE = 0x2178u
};

/* Lends the girl being played every swimsuit she can wear. */
void recomp_unlock_swimsuits_frame(void);
/* Takes the lent swimsuits back when the game hashes a save image. */
void recomp_unlock_swimsuits_hash(uint32_t length);
/* Brackets the game's save routine; nothing is lent while it runs. */
void recomp_unlock_swimsuits_saving(bool active);

#ifdef __cplusplus
}
#endif

#endif
