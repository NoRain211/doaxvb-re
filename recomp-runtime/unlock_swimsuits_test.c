/* Synthetic guest memory; no retail data or generated code. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#define RECOMP_FULL_PROGRAM
#include "unlock_swimsuits.c"

RecompRuntime recomp_runtime;
static uint8_t memory[RECOMP_XBOX_RAM_SIZE];
uint8_t *recomp_fast_ram;

uint8_t *recomp_memory(uint32_t address, size_t size)
{
    if (address > sizeof memory || size > sizeof memory - address) {
        fprintf(stderr, "Unlock memory out of bounds: address=%u size=%zu\n", address, size);
        abort();
    }
    return memory + address;
}
uint32_t *recomp_memory_u32_checked(uint32_t address) { return (uint32_t *)recomp_memory(address, 4); }
uint16_t *recomp_memory_u16(uint32_t address) { return (uint16_t *)recomp_memory(address, 2); }

enum {
    TEST_ITEMS = 0x00400000u,
    TEST_INVENTORY = 0x00500000u,
    TEST_SHADOW = 0x00600000u,
    TEST_READ_BUFFER = 0x00700000u,
    TEST_COLLECTION = RECOMP_SAVE_STATE_ADDRESS + 0x15E6u,
    OTHER_GIRL_SUIT = 5,
    OWN_SUIT = 1,
    KEPT_SUIT = 7,
    LENT_SUIT = 2,
    ACCESSORY = 0x258
};

uint32_t recomp_kernel_allocate_pool(uint32_t size)
{
    (void)size;
    return TEST_SHADOW;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Unlock line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static uint16_t *owned(uint16_t id) { return recomp_memory_u16(TEST_COLLECTION + id * 2u); }
static uint8_t *slot(unsigned index) { return recomp_memory(TEST_INVENTORY + index * SLOT_SIZE, SLOT_SIZE); }
static uint8_t *saved(unsigned index)
{
    return recomp_memory(RECOMP_SAVE_STATE_ADDRESS + SAVE_INVENTORY + index * SAVE_SLOT_SIZE,
        SAVE_SLOT_SIZE);
}

static void put(unsigned index, uint16_t id, uint8_t state, uint8_t order)
{
    const uint32_t item = id < SWIMSUIT_END ? TEST_ITEMS + id * ITEM_SIZE : 0u;
    uint8_t *entry = slot(index);

    memset(entry, 0, SLOT_SIZE);
    memcpy(entry, &id, sizeof id);
    entry[SLOT_STATE] = state;
    entry[SLOT_ORDER] = order;
    memcpy(entry + SLOT_ITEM, &item, sizeof item);
}

/* Packs the first girl's slots the way the game's save routine would. */
static void pack(void)
{
    for (unsigned i = 0; i < SLOT_COUNT; ++i) {
        memcpy(saved(i), slot(i), SAVE_SLOT_SIZE);
    }
}

int main(void)
{
    static const uint8_t characters[GIRL_COUNT] = {1, 5, 7, 12, 13, 14, 15, 20};
    uint8_t before[SLOT_COUNT * SAVE_SLOT_SIZE];
    uint32_t item;

#ifdef _WIN32
    _putenv_s("RECOMP_UNLOCK_SWIMSUITS", "1");
#else
    setenv("RECOMP_UNLOCK_SWIMSUITS", "1", 1);
#endif
    for (uint16_t id = 1u; id < SWIMSUIT_END; ++id) {
        uint8_t *definition = recomp_memory(TEST_ITEMS + id * ITEM_SIZE, ITEM_SIZE);
        const uint16_t wear = id == OTHER_GIRL_SUIT ? 2u : 0xFFFFu;

        definition[0] = ITEM_TYPE_SWIMSUIT;
        memcpy(definition + ITEM_WEAR_MASK, &wear, sizeof wear);
        *recomp_memory_u32(ITEM_TABLE + id * 4u) = TEST_ITEMS + id * ITEM_SIZE;
    }
    memcpy(recomp_memory(CHARACTER_IDS, GIRL_COUNT), characters, GIRL_COUNT);
    *recomp_memory(PLAYER_CHARACTER, 1u) = characters[0];
    *recomp_memory(SCENE, 1u) = SCENE_VACATION;
    *recomp_memory_u32(COLLECTION_POINTER) = TEST_COLLECTION;
    *recomp_memory_u32(INVENTORY_POINTER) = TEST_INVENTORY;
    memset(recomp_memory(TEST_INVENTORY, GIRL_COUNT * SLOT_COUNT * SLOT_SIZE), 0xFF,
        GIRL_COUNT * SLOT_COUNT * SLOT_SIZE);
    put(0, OWN_SUIT, 2, 0);
    put(1, ACCESSORY, 0, 3); /* a gap in the acquisition order */
    *owned(OWN_SUIT) = HAD;
    *owned(KEPT_SUIT) = HAD | IN_COLLECTION;

    /* Lending marks the played girl's wearable suits and nobody else's. */
    recomp_unlock_swimsuits_frame();
    CHECK(*owned(LENT_SUIT) == (HAD | IN_COLLECTION));
    CHECK(*owned(OTHER_GIRL_SUIT) == 0u);
    CHECK(*owned(OWN_SUIT) == HAD); /* held in the inventory */
    CHECK(*owned(KEPT_SUIT) == (HAD | IN_COLLECTION));
    CHECK((*recomp_memory_u16(GALLERY + OTHER_GIRL_SUIT * 2u)) == ((HAD | IN_COLLECTION) << 2));
    CHECK(*recomp_memory_u16(GALLERY_COUNTS) == SWIMSUIT_END - 2);
    CHECK(*recomp_memory_u16(GALLERY_COUNTS + 2u) == SWIMSUIT_END - 1);

    /* A save image hashed after a read takes the lent bits back and is left alone. */
    pack();
    memcpy(before, saved(0), sizeof before);
    recomp_unlock_swimsuits_hash(TEST_READ_BUFFER, RECOMP_SAVE_STATE_SIZE);
    CHECK(*owned(LENT_SUIT) == 0u);
    CHECK(*owned(OWN_SUIT) == HAD);
    CHECK(*owned(KEPT_SUIT) == (HAD | IN_COLLECTION));
    recomp_unlock_swimsuits_frame();
    recomp_unlock_swimsuits_hash(RECOMP_SAVE_STATE_ADDRESS, RECOMP_SAVE_STATE_SIZE);
    CHECK(*owned(LENT_SUIT) == 0u);
    CHECK(memcmp(before, saved(0), sizeof before) == 0);

    /* A save with no lent suit in the inventory keeps its order bytes. */
    recomp_unlock_swimsuits_frame();
    recomp_unlock_swimsuits_saving(true);
    recomp_unlock_swimsuits_hash(RECOMP_SAVE_STATE_ADDRESS, RECOMP_SAVE_STATE_SIZE);
    CHECK(*owned(LENT_SUIT) == 0u);
    CHECK(memcmp(before, saved(0), sizeof before) == 0);
    recomp_unlock_swimsuits_frame(); /* nothing is lent while saving */
    CHECK(*owned(LENT_SUIT) == 0u);
    recomp_unlock_swimsuits_saving(false);

    /* Take out: the game clears the Collection bit and fills a slot. */
    recomp_unlock_swimsuits_frame();
    CHECK(*owned(LENT_SUIT) == (HAD | IN_COLLECTION));
    *owned(LENT_SUIT) &= (uint16_t)~IN_COLLECTION;
    put(2, LENT_SUIT, 0, 4);
    recomp_unlock_swimsuits_frame();
    memcpy(&item, slot(2) + SLOT_ITEM, sizeof item);
    CHECK(item == TEST_SHADOW + (LENT_SUIT - 1u) * ITEM_SIZE);
    CHECK((*owned(LENT_SUIT) & IN_COLLECTION) == 0u);
    memcpy(&item, slot(0) + SLOT_ITEM, sizeof item);
    CHECK(item == TEST_ITEMS + OWN_SUIT * ITEM_SIZE);

    /* Saving blanks the lent plain suit and closes up the order. */
    pack();
    recomp_unlock_swimsuits_saving(true);
    recomp_unlock_swimsuits_hash(RECOMP_SAVE_STATE_ADDRESS, RECOMP_SAVE_STATE_SIZE);
    recomp_unlock_swimsuits_saving(false);
    CHECK(slot_id(saved(2)) == EMPTY_ID);
    CHECK(slot_id(saved(0)) == OWN_SUIT && saved(0)[SAVE_SLOT_ORDER] == 0u);
    CHECK(slot_id(saved(1)) == ACCESSORY && saved(1)[SAVE_SLOT_ORDER] == 1u);
    CHECK(*owned(LENT_SUIT) == 0u);
    CHECK(*owned(3) == 0u);

    /* A worn lent suit is saved. */
    recomp_unlock_swimsuits_frame();
    slot(2)[SLOT_STATE] = 2;
    pack();
    recomp_unlock_swimsuits_saving(true);
    recomp_unlock_swimsuits_hash(RECOMP_SAVE_STATE_ADDRESS, RECOMP_SAVE_STATE_SIZE);
    recomp_unlock_swimsuits_saving(false);
    CHECK(slot_id(saved(2)) == LENT_SUIT);
    slot(2)[SLOT_STATE] = 0;

    /* Keep: the slot empties and the game sets the bit, which stays ours. */
    recomp_unlock_swimsuits_frame();
    memset(slot(2), 0xFF, SLOT_SIZE);
    *owned(LENT_SUIT) |= IN_COLLECTION;
    recomp_unlock_swimsuits_frame();
    recomp_unlock_swimsuits_hash(TEST_READ_BUFFER, RECOMP_SAVE_STATE_SIZE);
    CHECK(*owned(LENT_SUIT) == 0u);

    /* Outside a vacation the Collection is left as the game has it. */
    recomp_unlock_swimsuits_frame();
    CHECK(*owned(LENT_SUIT) != 0u);
    *recomp_memory(SCENE, 1u) = 0;
    recomp_unlock_swimsuits_frame();
    CHECK(*owned(LENT_SUIT) == 0u);
    CHECK(*owned(KEPT_SUIT) == (HAD | IN_COLLECTION));
    return 0;
}
