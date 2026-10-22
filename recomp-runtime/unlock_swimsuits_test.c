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

enum {
    TEST_ITEMS = 0x00400000u,
    VACATION_TABLE = 0x009D8DCEu, /* stands in for the saved ownership table */
    OTHER_GIRL_SUIT = 5
};

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Unlock line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
    uint8_t vacation[SWIMSUIT_END * 2u];

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
    for (unsigned i = 0; i < sizeof vacation; ++i) {
        vacation[i] = (uint8_t)(i * 7u);
    }
    memcpy(recomp_memory(VACATION_TABLE, sizeof vacation), vacation, sizeof vacation);
    /* Girl 0 has had the other girl's suit in a save; that bit stays. */
    *recomp_memory_u16(GALLERY + OTHER_GIRL_SUIT * 2u) = HAD;

    for (unsigned frame = 0; frame < 2; ++frame) {
        recomp_unlock_swimsuits_frame();
        CHECK(*recomp_memory_u16(GALLERY + 1u * 2u) == 0xFFFFu);
        CHECK(*recomp_memory_u16(GALLERY + (SWIMSUIT_END - 1u) * 2u) == 0xFFFFu);
        CHECK(*recomp_memory_u16(GALLERY + OTHER_GIRL_SUIT * 2u) == (HAD | ((HAD | IN_COLLECTION) << 2)));
        CHECK(*recomp_memory_u16(GALLERY_COUNTS) == SWIMSUIT_END - 2);
        CHECK(*recomp_memory_u16(GALLERY_COUNTS + 2u) == SWIMSUIT_END - 1);
        CHECK(*recomp_memory_u16(GALLERY_COUNTS + 14u) == SWIMSUIT_END - 2);
        /* Vacation ownership and saves are never touched. */
        CHECK(memcmp(recomp_memory(VACATION_TABLE, sizeof vacation), vacation, sizeof vacation) == 0);
    }
    puts("PASS Exhibition swimsuit unlock");
    return 0;
}
