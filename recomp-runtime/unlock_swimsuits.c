#include "unlock_swimsuits.h"

#ifdef RECOMP_FULL_PROGRAM
#include "runtime.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exhibition swimsuit unlock, opt-in with RECOMP_UNLOCK_SWIMSUITS=1.

   Exhibition offers each girl the suits in a table of 708 words, one per
   item, at 0x9DAE08. 0x000F82F0 rebuilds it from the default suits and all
   three save files, and counts each girl's suits into the eight words at
   0x9DB3C8. A girl has two bits in a word, at twice her inventory index: the
   low one says she has had the item, the high one that it is in her
   Collection. The table is never saved, so every swimsuit (ids 1..0x148) a
   girl can wear is marked in it each frame. Vacation ownership, the
   inventories and the saves are never touched. */
enum {
    GALLERY = 0x009DAE08u,
    GALLERY_COUNTS = 0x009DB3C8u,
    ITEM_TABLE = 0x003003E8u,
    GIRL_COUNT = 8,
    ITEM_SIZE = 28,
    ITEM_WEAR_MASK = 16,
    ITEM_TYPE_SWIMSUIT = 2,
    SWIMSUIT_END = 0x149,
    HAD = 1,
    IN_COLLECTION = 2
};

static uint16_t wear_mask[SWIMSUIT_END];

static bool enabled(void)
{
    static int setting = -1;

    if (setting < 0) {
        const char *value = getenv("RECOMP_UNLOCK_SWIMSUITS");

        setting = value != NULL && strcmp(value, "1") == 0;
    }
    return setting != 0;
}

/* Reads each swimsuit's wear mask. Returns false, leaving the feature off,
   when the image does not look as expected. */
static bool prepare(void)
{
    static int state;

    if (state != 0) {
        return state > 0;
    }
    state = -1;
    for (uint16_t id = 1u; id < SWIMSUIT_END; ++id) {
        const uint32_t item = *recomp_memory_u32(ITEM_TABLE + id * 4u);
        const uint8_t *source = item != 0u ? recomp_memory(item, ITEM_SIZE) : NULL;

        if (source == NULL || source[0] != ITEM_TYPE_SWIMSUIT || source[1] != 0u) {
            fprintf(stderr, "[unlock-swimsuits] item %u is not a swimsuit; disabled\n", id);
            return false;
        }
        memcpy(&wear_mask[id], source + ITEM_WEAR_MASK, sizeof wear_mask[id]);
    }
    state = 1;
    return true;
}

void recomp_unlock_swimsuits_frame(void)
{
    uint16_t counts[GIRL_COUNT] = {0};

    if (!enabled() || !prepare()) {
        return;
    }
    for (uint16_t id = 1u; id < SWIMSUIT_END; ++id) {
        uint16_t owned = 0;

        for (unsigned girl = 0; girl < GIRL_COUNT; ++girl) {
            if ((wear_mask[id] & (1u << girl)) != 0u) {
                owned |= (uint16_t)((HAD | IN_COLLECTION) << (girl * 2u));
                ++counts[girl];
            }
        }
        *recomp_memory_u16(GALLERY + id * 2u) |= owned;
    }
    memcpy(recomp_memory(GALLERY_COUNTS, sizeof counts), counts, sizeof counts);
}
#else
void recomp_unlock_swimsuits_frame(void) {}
#endif
