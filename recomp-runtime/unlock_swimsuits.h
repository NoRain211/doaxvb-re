#ifndef DOAXBV_RECOMP_UNLOCK_SWIMSUITS_H
#define DOAXBV_RECOMP_UNLOCK_SWIMSUITS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The game's save image and its size; the save code signs it in one piece. */
enum {
    RECOMP_SAVE_STATE_ADDRESS = 0x009D77E8u,
    RECOMP_SAVE_STATE_SIZE = 0x2178u
};

/* Tops up every girl's free inventory slots with her missing swimsuits. */
void recomp_unlock_swimsuits_frame(void);
/* Removes granted swimsuits from a save image about to be signed. */
void recomp_unlock_swimsuits_strip(uint8_t *save_state);

#ifdef __cplusplus
}
#endif

#endif
