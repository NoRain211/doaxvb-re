#ifndef DOAXBV_RECOMP_UNLOCK_SWIMSUITS_H
#define DOAXBV_RECOMP_UNLOCK_SWIMSUITS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The game's save image and its size; the save code signs it in one piece. */
enum {
    RECOMP_SAVE_STATE_ADDRESS = 0x009D77E8u,
    RECOMP_SAVE_STATE_SIZE = 0x2178u
};

/* Lends the girl being played every swimsuit she can wear. */
void recomp_unlock_swimsuits_frame(void);
/* Takes the lent swimsuits back out of a save image the game is hashing. */
void recomp_unlock_swimsuits_hash(uint32_t address, uint32_t length);
/* Brackets the game's save routine; nothing is lent while it runs. */
void recomp_unlock_swimsuits_saving(bool active);

#ifdef __cplusplus
}
#endif

#endif
