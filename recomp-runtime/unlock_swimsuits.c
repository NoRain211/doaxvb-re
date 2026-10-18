#include "unlock_swimsuits.h"

#ifdef RECOMP_FULL_PROGRAM
#include "runtime.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Swimsuit unlock, opt-in with RECOMP_UNLOCK_SWIMSUITS=1.

   The game records what each girl has in a table of 708 words, one per item,
   at the pointer in 0xB5B244. A girl has two bits there: the low one says she
   has had the item, the high one that it is in her Collection, the unlimited
   store she fills with Keep and empties with Take out at the hotel. Her
   inventory, eight of which sit at the pointer in 0x317634, holds only 51
   slots of 16 bytes, each starting with the item id (0xFFFF when empty).

   Each frame the girl being played gets the Collection bits of every
   swimsuit (ids 1..0x148) her wear mask allows and she holds in neither
   place. `lent` remembers which bits are ours. A suit she takes out is hers
   from then on: the game clears the bit and fills an inventory slot, and
   nothing is lent, or remembered as lent, for a suit she holds. `held`
   marks each suit seen in her inventory or taken out, and a held suit is not
   lent again until the vacation ends or another girl is played: Keep and
   Take out may move a suit between the two places over two frames.

   The table is part of the save image at 0x9D77E8. Whenever the game hashes
   an image of that size, to sign a save or to check one it has read, the
   lent bits are withdrawn from the live table, so a save holds the
   Collection the player built and the inventory as it stands. Nothing is
   lent again until the save routine has returned, and nothing is lent
   outside a vacation (scene byte 0x5DECE0), where the game reads saves into
   the table.

   Outside a vacation the game offers the suits in a second table of the
   same shape at 0x9DAE08, which 0x000F82F0 rebuilds from the default suits
   and all three save files, counting each girl's suits into the eight words
   at 0x9DB3C8. That table is never saved, so every girl's wearable suits are
   simply marked in it each frame. */
enum {
    INVENTORY_POINTER = 0x00317634u,
    COLLECTION_POINTER = 0x00B5B244u,
    SCENE = 0x005DECE0u,
    SCENE_VACATION = 1,
    PLAYER_CHARACTER = 0x009D9939u,
    CHARACTER_IDS = 0x002A7B00u,
    GALLERY = 0x009DAE08u,
    GALLERY_COUNTS = 0x009DB3C8u,
    ITEM_TABLE = 0x003003E8u,
    ITEM_COUNT = 0x2C4,
    GIRL_COUNT = 8,
    SLOT_COUNT = 51,
    SLOT_SIZE = 16,
    ITEM_SIZE = 28,
    ITEM_WEAR_MASK = 16,
    ITEM_TYPE_SWIMSUIT = 2,
    SWIMSUIT_END = 0x149,
    EMPTY_ID = 0xFFFF,
    HAD = 1,
    IN_COLLECTION = 2
};

static uint16_t wear_mask[SWIMSUIT_END];
static uint16_t lent[SWIMSUIT_END];
static bool held[SWIMSUIT_END];
static unsigned lent_girl;
static unsigned saving;

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

/* The inventory index of the girl being played, or GIRL_COUNT. */
static unsigned player_girl(void)
{
    const uint8_t character = *recomp_memory(PLAYER_CHARACTER, 1u);
    const uint8_t *ids = recomp_memory(CHARACTER_IDS, GIRL_COUNT);
    unsigned girl = 0;

    while (girl < GIRL_COUNT && ids[girl] != character) {
        ++girl;
    }
    return girl;
}

static void withdraw(void)
{
    const uint32_t collection = *recomp_memory_u32(COLLECTION_POINTER);

    if (collection != 0u) {
        for (uint16_t id = 1u; id < SWIMSUIT_END; ++id) {
            *recomp_memory_u16(collection + id * 2u) &= (uint16_t)~lent[id];
        }
    }
    memset(lent, 0, sizeof lent);
}

static void lend_gallery(void)
{
    uint16_t counts[GIRL_COUNT] = {0};

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

void recomp_unlock_swimsuits_frame(void)
{
    bool in_bag[SWIMSUIT_END] = {false};
    uint32_t inventory;
    uint32_t collection;
    const uint8_t *slots;
    unsigned girl;

    if (!enabled() || !prepare()) {
        return;
    }
    lend_gallery();
    if (saving != 0u) {
        return;
    }
    if (*recomp_memory(SCENE, 1u) != SCENE_VACATION) {
        withdraw();
        memset(held, 0, sizeof held);
        return;
    }
    inventory = *recomp_memory_u32(INVENTORY_POINTER);
    collection = *recomp_memory_u32(COLLECTION_POINTER);
    girl = player_girl();
    if (inventory == 0u || collection == 0u || girl == GIRL_COUNT) {
        return;
    }
    if (girl != lent_girl) {
        withdraw();
        memset(held, 0, sizeof held);
        lent_girl = girl;
    }
    slots = recomp_memory(inventory + girl * SLOT_COUNT * SLOT_SIZE, SLOT_COUNT * SLOT_SIZE);
    for (unsigned i = 0; i < SLOT_COUNT; ++i) {
        uint16_t id;

        memcpy(&id, slots + i * SLOT_SIZE, sizeof id);
        if (id == EMPTY_ID) {
            continue;
        }
        if (id >= ITEM_COUNT) {
            return; /* not an initialized inventory */
        }
        if (id < SWIMSUIT_END) {
            in_bag[id] = true;
        }
    }
    for (uint16_t id = 1u; id < SWIMSUIT_END; ++id) {
        const uint16_t had = (uint16_t)(HAD << (girl * 2u));
        const uint16_t in_collection = (uint16_t)(IN_COLLECTION << (girl * 2u));
        uint16_t *owned = recomp_memory_u16(collection + id * 2u);

        if ((wear_mask[id] & (1u << girl)) == 0u) {
            continue;
        }
        if (in_bag[id]) {
            /* Hers now, whether bought or taken out. */
            lent[id] &= (uint16_t)~(in_collection | had);
            held[id] = true;
        }
        if ((*owned & in_collection) == 0u) {
            if ((lent[id] & in_collection) != 0u) {
                /* Taken out, or the table was reloaded: the bit is no longer ours. */
                lent[id] &= (uint16_t)~in_collection;
                held[id] = true;
            }
            if (!held[id]) {
                lent[id] |= in_collection | (uint16_t)(~*owned & had);
                *owned |= in_collection | had;
            }
        }
    }
}

void recomp_unlock_swimsuits_hash(uint32_t length)
{
    if (enabled() && length == RECOMP_SAVE_STATE_SIZE) {
        withdraw();
    }
}

void recomp_unlock_swimsuits_saving(bool active)
{
    /* A region save nests inside the profile save. */
    if (active) {
        ++saving;
    } else if (saving != 0u) {
        --saving;
    }
}
#else
void recomp_unlock_swimsuits_frame(void) {}
void recomp_unlock_swimsuits_hash(uint32_t length) { (void)length; }
void recomp_unlock_swimsuits_saving(bool active) { (void)active; }
#endif
