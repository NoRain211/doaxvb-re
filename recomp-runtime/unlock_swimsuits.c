#include "unlock_swimsuits.h"

#ifdef RECOMP_FULL_PROGRAM
#include "kernel_abi.h"
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
   slots of 16 bytes: item id (0xFFFF when empty), two model handles, a state
   byte (0 plain, 2 worn, others wrapped), the acquisition order, a wrap item,
   a pointer to the item's 28-byte definition and a counter.

   Each frame the girl being played gets the Collection bits of every
   swimsuit (ids 1..0x148) her wear mask allows and she holds in neither
   place. `lent` remembers which bits are ours. When the game clears a lent
   Collection bit the suit has been taken out, so its inventory slot is
   pointed at a private copy of the item definition. The game rewrites that
   pointer whenever it loads, clears or fills a slot itself, so the pointer
   alone marks a lent suit in the inventory.

   The table is part of the save image at 0x9D77E8. Whenever the game hashes
   an image of that size, to sign a save or to check one it has read, the
   lent bits are withdrawn from the live table, and from a save in progress
   the lent plain suits are blanked and the remaining order indices closed
   up. A save therefore holds only what the player owns; a lent suit a girl
   is wearing or has wrapped as a gift stays, because the game refers to
   it. Nothing is lent again until the save routine has returned, and
   nothing is lent outside a vacation (scene byte 0x5DECE0), where the game
   reads saves into the table. */
enum {
    INVENTORY_POINTER = 0x00317634u,
    COLLECTION_POINTER = 0x00B5B244u,
    SCENE = 0x005DECE0u,
    SCENE_VACATION = 1,
    PLAYER_CHARACTER = 0x009D9939u,
    CHARACTER_IDS = 0x002A7B00u,
    ITEM_TABLE = 0x003003E8u,
    GIRL_COUNT = 8,
    SLOT_COUNT = 51,
    SLOT_SIZE = 16,
    SLOT_STATE = 4,
    SLOT_ORDER = 5,
    SLOT_ITEM = 8,
    ITEM_SIZE = 28,
    ITEM_WEAR_MASK = 16,
    ITEM_TYPE_SWIMSUIT = 2,
    SWIMSUIT_END = 0x149,
    EMPTY_ID = 0xFFFF,
    HAD = 1,
    IN_COLLECTION = 2,
    SAVE_INVENTORY = 0x28,
    SAVE_SLOT_SIZE = 8,
    SAVE_SLOT_ORDER = 5
};

static uint32_t shadow_items;
static uint16_t lent[SWIMSUIT_END];
static bool lent_in_bag[SWIMSUIT_END];
static unsigned lent_girl;
static bool saving;

static bool enabled(void)
{
    static int setting = -1;

    if (setting < 0) {
        const char *value = getenv("RECOMP_UNLOCK_SWIMSUITS");

        setting = value != NULL && strcmp(value, "1") == 0;
    }
    return setting != 0;
}

static uint32_t shadow_item(uint16_t id)
{
    return shadow_items + (uint32_t)(id - 1u) * ITEM_SIZE;
}

static bool is_lent(const uint8_t *slot)
{
    uint32_t item;

    memcpy(&item, slot + SLOT_ITEM, sizeof item);
    return shadow_items != 0u && item >= shadow_items &&
        item < shadow_items + (SWIMSUIT_END - 1u) * ITEM_SIZE;
}

static uint16_t slot_id(const uint8_t *slot)
{
    uint16_t id;

    memcpy(&id, slot, sizeof id);
    return id;
}

/* Copies the swimsuit definitions. Returns false, leaving the feature off,
   when the image does not look as expected. */
static bool prepare(void)
{
    static int state;

    if (state != 0) {
        return state > 0;
    }
    state = -1;
    shadow_items = recomp_kernel_allocate_pool((SWIMSUIT_END - 1u) * ITEM_SIZE);
    if (shadow_items == 0u) {
        fprintf(stderr, "[unlock-swimsuits] no guest memory for the item copies; disabled\n");
        return false;
    }
    for (uint16_t id = 1u; id < SWIMSUIT_END; ++id) {
        const uint32_t item = *recomp_memory_u32(ITEM_TABLE + id * 4u);
        const uint8_t *source = item != 0u ? recomp_memory(item, ITEM_SIZE) : NULL;

        if (source == NULL || source[0] != ITEM_TYPE_SWIMSUIT || source[1] != 0u) {
            fprintf(stderr, "[unlock-swimsuits] item %u is not a swimsuit; disabled\n", id);
            shadow_items = 0u;
            return false;
        }
        memcpy(recomp_memory(shadow_item(id), ITEM_SIZE), source, ITEM_SIZE);
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

void recomp_unlock_swimsuits_frame(void)
{
    bool in_bag[SWIMSUIT_END] = {false};
    bool lent_now[SWIMSUIT_END] = {false};
    uint8_t *newest[SWIMSUIT_END] = {NULL};
    uint32_t inventory;
    uint32_t collection;
    uint8_t *slots;
    unsigned girl;

    if (!enabled() || saving) {
        return;
    }
    if (*recomp_memory(SCENE, 1u) != SCENE_VACATION) {
        withdraw();
        return;
    }
    inventory = *recomp_memory_u32(INVENTORY_POINTER);
    collection = *recomp_memory_u32(COLLECTION_POINTER);
    girl = player_girl();
    if (inventory == 0u || collection == 0u || girl == GIRL_COUNT || !prepare()) {
        return;
    }
    if (girl != lent_girl) {
        withdraw();
        memset(lent_in_bag, 0, sizeof lent_in_bag);
        lent_girl = girl;
    }
    slots = recomp_memory(inventory + girl * SLOT_COUNT * SLOT_SIZE, SLOT_COUNT * SLOT_SIZE);
    for (unsigned i = 0; i < SLOT_COUNT; ++i) {
        uint8_t *slot = slots + i * SLOT_SIZE;
        const uint16_t id = slot_id(slot);

        if (id == EMPTY_ID) {
            continue;
        }
        if (id >= 0x2C4u) {
            return; /* not an initialized inventory */
        }
        if (id < SWIMSUIT_END) {
            in_bag[id] = true;
            if (is_lent(slot)) {
                lent_now[id] = true;
            } else if (newest[id] == NULL || slot[SLOT_ORDER] > newest[id][SLOT_ORDER]) {
                newest[id] = slot;
            }
        }
    }
    for (uint16_t id = 1u; id < SWIMSUIT_END; ++id) {
        const uint16_t had = (uint16_t)(HAD << (girl * 2u));
        const uint16_t in_collection = (uint16_t)(IN_COLLECTION << (girl * 2u));
        const uint32_t item = shadow_item(id);
        uint16_t *owned = recomp_memory_u16(collection + id * 2u);

        if ((*recomp_memory_u16(item + ITEM_WEAR_MASK) & (1u << girl)) == 0u) {
            continue;
        }
        if ((lent[id] & in_collection) != 0u && (*owned & in_collection) == 0u) {
            /* Taken out, or the table was reloaded. */
            lent[id] &= (uint16_t)~in_collection;
            if (newest[id] != NULL && !lent_now[id]) {
                memcpy(newest[id] + SLOT_ITEM, &item, sizeof item);
                lent_now[id] = true;
            }
        } else if ((lent[id] & in_collection) == 0u && (*owned & in_collection) != 0u &&
                   lent_in_bag[id] && !lent_now[id]) {
            /* A lent suit was put back with Keep. */
            lent[id] |= in_collection;
        }
        if ((*owned & in_collection) == 0u && !in_bag[id]) {
            lent[id] |= in_collection | (uint16_t)(~*owned & had);
            *owned |= in_collection | had;
        }
        lent_in_bag[id] = lent_now[id];
    }
}

void recomp_unlock_swimsuits_hash(uint32_t address, uint32_t length)
{
    uint32_t inventory;
    uint8_t *saved;
    const uint8_t *slots;
    uint8_t kept[SLOT_COUNT];
    unsigned kept_count = 0;

    if (!enabled() || shadow_items == 0u || length != RECOMP_SAVE_STATE_SIZE) {
        return;
    }
    withdraw();
    inventory = *recomp_memory_u32(INVENTORY_POINTER);
    if (address != RECOMP_SAVE_STATE_ADDRESS || inventory == 0u) {
        return;
    }
    slots = recomp_memory(inventory + lent_girl * SLOT_COUNT * SLOT_SIZE, SLOT_COUNT * SLOT_SIZE);
    saved = recomp_memory(address + SAVE_INVENTORY + lent_girl * SLOT_COUNT * SAVE_SLOT_SIZE,
        SLOT_COUNT * SAVE_SLOT_SIZE);
    for (unsigned i = 0; i < SLOT_COUNT; ++i) {
        const uint8_t *slot = slots + i * SLOT_SIZE;
        uint8_t *entry = saved + i * SAVE_SLOT_SIZE;

        if (is_lent(slot) && slot[SLOT_STATE] == 0u &&
            memcmp(entry, slot, sizeof(uint16_t)) == 0) {
            static const uint8_t empty[SAVE_SLOT_SIZE] =
                {0xFFu, 0xFFu, 0u, 0u, 0u, 0xFFu, 0u, 0u};

            memcpy(entry, empty, sizeof empty);
        } else if (slot_id(entry) != EMPTY_ID) {
            kept[kept_count++] = (uint8_t)i;
        }
    }
    /* Close the gaps the removed suits leave in the acquisition order. */
    for (unsigned order = 0; order < kept_count; ++order) {
        unsigned lowest = order;

        for (unsigned j = order + 1u; j < kept_count; ++j) {
            if (saved[kept[j] * SAVE_SLOT_SIZE + SAVE_SLOT_ORDER] <
                saved[kept[lowest] * SAVE_SLOT_SIZE + SAVE_SLOT_ORDER]) {
                lowest = j;
            }
        }
        const uint8_t swap = kept[order];

        kept[order] = kept[lowest];
        kept[lowest] = swap;
        saved[kept[order] * SAVE_SLOT_SIZE + SAVE_SLOT_ORDER] = (uint8_t)order;
    }
}

void recomp_unlock_swimsuits_saving(bool active)
{
    saving = active;
}
#else
void recomp_unlock_swimsuits_frame(void) {}
void recomp_unlock_swimsuits_hash(uint32_t address, uint32_t length)
{
    (void)address;
    (void)length;
}
void recomp_unlock_swimsuits_saving(bool active) { (void)active; }
#endif
