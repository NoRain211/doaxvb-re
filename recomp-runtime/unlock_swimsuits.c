#include "unlock_swimsuits.h"

#ifdef RECOMP_FULL_PROGRAM
#include "kernel_abi.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Swimsuit unlock, opt-in with RECOMP_UNLOCK_SWIMSUITS=1.

   The game keeps eight inventories of 51 slots at the pointer in 0x317634.
   A slot is 16 bytes: item id (0xFFFF when empty), two model handles, a state
   byte (0 plain, 2 worn, others wrapped), the acquisition order, a wrap item,
   a pointer to the item's 28-byte definition and a counter. Swimsuits are
   ids 1..0x148, grouped per girl; each group starts at her default suit in
   the table at 0x2758A4.

   Each frame every girl's free slots are topped up with the suits of her
   group she lacks. The game's own add routine (0x0009CEA0) only uses the
   first 48 slots, so the same limit applies here, less a few slots kept free
   so shops and gifts still work. A girl with more suits than room gets the
   lowest ids.

   A granted slot points at a private copy of its item definition. The game
   rewrites that pointer whenever it loads, clears or fills a slot itself, so
   the tag never outlives the grant. When the game signs its save image
   (0x9D77E8, packed from the slots by 0x000E6BA0), tagged plain suits are
   blanked from the image and the remaining order indices are closed up. A
   save therefore holds only what the player owns; a granted suit a girl is
   wearing or has wrapped as a gift stays, because the game refers to it. */
enum {
    INVENTORY_POINTER = 0x00317634u,
    ITEM_TABLE = 0x003003E8u,
    DEFAULT_SUITS = 0x002758A4u,
    GIRL_COUNT = 8,
    SLOT_COUNT = 51,
    ADD_SLOT_COUNT = 48,
    FREE_SLOTS_KEPT = 3,
    SLOT_SIZE = 16,
    SLOT_HANDLES = 2,
    SLOT_STATE = 4,
    SLOT_ORDER = 5,
    SLOT_ITEM = 8,
    SLOT_COUNTER = 12,
    ITEM_SIZE = 28,
    ITEM_COUNTER = 12,
    ITEM_TYPE_SWIMSUIT = 2,
    SWIMSUIT_END = 0x149,
    EMPTY_ID = 0xFFFF,
    SAVE_INVENTORY = 0x28,
    SAVE_SLOT_SIZE = 8,
    SAVE_SLOT_ORDER = 5
};

static uint32_t shadow_items;
static uint16_t first_suit[GIRL_COUNT + 1];

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

static bool granted(const uint8_t *slot)
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

/* Copies the swimsuit definitions and reads each girl's id range. Returns
   false, leaving the feature off, when the image does not look as expected. */
static bool prepare(void)
{
    static int state;

    if (state != 0) {
        return state > 0;
    }
    state = -1;
    for (unsigned girl = 0; girl < GIRL_COUNT; ++girl) {
        first_suit[girl] = *recomp_memory_u16(DEFAULT_SUITS + girl * 4u);
        if (first_suit[girl] == 0u || first_suit[girl] >= SWIMSUIT_END ||
            (girl != 0u && first_suit[girl] <= first_suit[girl - 1u])) {
            fprintf(stderr, "[unlock-swimsuits] unexpected default suit table; disabled\n");
            return false;
        }
    }
    first_suit[GIRL_COUNT] = SWIMSUIT_END;
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

static void grant(uint8_t *slots, unsigned girl)
{
    bool owned[SWIMSUIT_END] = {false};
    unsigned used = 0;

    for (unsigned i = 0; i < SLOT_COUNT; ++i) {
        const uint16_t id = slot_id(slots + i * SLOT_SIZE);

        if (id == EMPTY_ID) {
            continue;
        }
        if (id >= 0x2C4u) {
            return; /* not an initialized inventory */
        }
        ++used;
        if (id < SWIMSUIT_END) {
            owned[id] = true;
        }
    }
    for (uint16_t id = first_suit[girl]; id < first_suit[girl + 1u]; ++id) {
        uint8_t *slot = NULL;

        if (owned[id]) {
            continue;
        }
        if (used + FREE_SLOTS_KEPT >= ADD_SLOT_COUNT) {
            return;
        }
        for (unsigned i = 0; i < ADD_SLOT_COUNT && slot == NULL; ++i) {
            if (slot_id(slots + i * SLOT_SIZE) == EMPTY_ID) {
                slot = slots + i * SLOT_SIZE;
            }
        }
        if (slot == NULL) {
            return;
        }
        const uint32_t item = shadow_item(id);
        const int16_t counter = (int8_t)*recomp_memory(item + ITEM_COUNTER, 1u);

        memset(slot, 0, SLOT_SIZE);
        memcpy(slot, &id, sizeof id);
        slot[SLOT_HANDLES] = 0xFFu;
        slot[SLOT_HANDLES + 1] = 0xFFu;
        slot[SLOT_ORDER] = (uint8_t)used;
        memcpy(slot + SLOT_ITEM, &item, sizeof item);
        memcpy(slot + SLOT_COUNTER, &counter, sizeof counter);
        ++used;
    }
}

void recomp_unlock_swimsuits_frame(void)
{
    uint32_t inventory;

    if (!enabled()) {
        return;
    }
    inventory = *recomp_memory_u32(INVENTORY_POINTER);
    if (inventory == 0u || !prepare()) {
        return;
    }
    for (unsigned girl = 0; girl < GIRL_COUNT; ++girl) {
        grant(recomp_memory(inventory + girl * SLOT_COUNT * SLOT_SIZE,
                  SLOT_COUNT * SLOT_SIZE), girl);
    }
}

void recomp_unlock_swimsuits_strip(uint8_t *save_state)
{
    uint32_t inventory;

    if (!enabled() || shadow_items == 0u) {
        return;
    }
    inventory = *recomp_memory_u32(INVENTORY_POINTER);
    if (inventory == 0u) {
        return;
    }
    for (unsigned girl = 0; girl < GIRL_COUNT; ++girl) {
        const uint8_t *slots = recomp_memory(
            inventory + girl * SLOT_COUNT * SLOT_SIZE, SLOT_COUNT * SLOT_SIZE);
        uint8_t *saved = save_state + SAVE_INVENTORY + girl * SLOT_COUNT * SAVE_SLOT_SIZE;
        uint8_t kept[SLOT_COUNT];
        unsigned kept_count = 0;

        for (unsigned i = 0; i < SLOT_COUNT; ++i) {
            const uint8_t *slot = slots + i * SLOT_SIZE;
            uint8_t *entry = saved + i * SAVE_SLOT_SIZE;

            if (granted(slot) && slot[SLOT_STATE] == 0u &&
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
}
#else
void recomp_unlock_swimsuits_frame(void) {}
void recomp_unlock_swimsuits_strip(uint8_t *save_state) { (void)save_state; }
#endif
