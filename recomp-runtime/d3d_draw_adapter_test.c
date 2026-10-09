#include "d3d_draw_adapter.c"

#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "draw adapter line %d\n", __LINE__); return 0; } } while (0)

int recomp_d3d_draw_adapter_test(void)
{
    uint8_t memory[0x7000] = {0};
    const RecompMemoryRegion region = {0x001f0000u, sizeof memory, memory};
    const uint32_t device = 0x001f3120u, resource = 0x001f6000u;
    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    recomp_d3d_texture_adapter_reset();
    *recomp_memory_u32(0x001f2978u) = device;
    *recomp_memory_u32(resource) = 0x00100001u;
    *recomp_memory_u32(resource + 4u) = resource + 0x100u;
    *recomp_memory_u32(resource + 12u) = 0x00000620u;
    *recomp_memory_i8(0x001f16b8u + 6u) = 0x20u;
    recomp_runtime.registers.esp = 0x001f6500u;
    *recomp_memory_u32(0x001f6504u) = 1u;
    *recomp_memory_u32(0x001f6508u) = resource;
    recomp_d3d_texture_lookup_manual(0x001e43f0u)();
    uint32_t (*stages)[32] = (uint32_t (*)[32])(void *)(memory + 0x2988u);
    const uint32_t words[2][8] = {{2, 0, 0, 1, 2, 0, 0, 1}, {23, 1, 2, 2, 2, 0, 1, 0}};
    memcpy(stages[0] + 12, words[0], sizeof words[0]);
    memcpy(stages[1] + 12, words[1], sizeof words[1]);
    stages[2][12] = 1;
    stages[1][0] = 3; stages[1][1] = 2;
    float bias = -2.0f;
    memcpy(&stages[1][6], &bias, sizeof bias);
    RecompD3dPresenterDrawCommand draw = {0};
    draw.fvf = 0x142u;
    attach_combiner(device, &draw);
    REQUIRE(draw.has_combiner && memcmp(draw.combiner, words, sizeof words) == 0);
    REQUIRE(draw.reflection_bytes != NULL && draw.combiner_address_u == 3 && draw.combiner_address_v == 2);
    REQUIRE(draw.combiner_lod_bias == -2.0f);
    for (uint32_t i = 0; i < 16; ++i) REQUIRE(draw.reflection_transform[i] == (i % 5 == 0 ? 1.0f : 0.0f));
    stages[1][21] = 2;
    float *transform = (float *)(void *)(memory + device - region.address + 0x810u + 3u * 0x40u);
    transform[0] = transform[5] = transform[10] = transform[15] = 1;
    transform[12] = 0.25f;
    draw.has_combiner = false;
    attach_combiner(device, &draw);
    REQUIRE(draw.has_combiner && draw.reflection_transform[12] == 0.25f);
    stages[1][21] = 0;
    const uint32_t rejected[][3] = {
        {0,12,17}, {0,16,17}, {1,12,17}, {1,16,17},
        {0,20,1}, {1,20,1}, {1,28,1}, {1,21,3},
        {2,12,2}, {1,12,25}, {0,14,5}, {0,14,0x40}
    };
    for (uint32_t i = 0; i < sizeof rejected / sizeof rejected[0]; ++i) {
        uint32_t *word = &stages[rejected[i][0]][rejected[i][1]], old = *word;
        *word = rejected[i][2]; draw.has_combiner = false;
        attach_combiner(device, &draw);
        REQUIRE(!draw.has_combiner);
        *word = old;
    }
    stages[0][14] = 4;
    draw.fvf = 0x1c2u; draw.has_combiner = false;
    attach_combiner(device, &draw);
    REQUIRE(!draw.has_combiner);
    draw.fvf = 0x142u;
    attach_combiner(device, &draw);
    REQUIRE(draw.has_combiner);
    recomp_runtime_init(NULL, 0u, NULL, 0u, NULL, 0u);
    return 1;
}
