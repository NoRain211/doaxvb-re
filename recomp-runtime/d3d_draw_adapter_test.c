#include "d3d_draw_adapter.c"

#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "draw adapter line %d\n", __LINE__); goto cleanup; } } while (0)

int recomp_d3d_draw_adapter_test(void)
{
    int passed = 0;
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
    RecompFunction set_texture = recomp_d3d_texture_lookup_manual(0x001e43f0u);
    REQUIRE(set_texture != NULL);
    set_texture();
    uint32_t (*stages)[32] = (uint32_t (*)[32])(void *)(memory + 0x2988u);
    const uint32_t words[2][8] = {{2, 0, 0, 1, 2, 0, 0, 1}, {23, 1, 2, 2, 2, 0, 1, 0}};
    memcpy(stages[0] + 12, words[0], sizeof words[0]);
    memcpy(stages[1] + 12, words[1], sizeof words[1]);
    stages[2][12] = 1;
    stages[1][0] = 3; stages[1][1] = 2;
    stages[1][3] = stages[1][4] = stages[1][5] = 2;
    float bias = -2.0f;
    memcpy(&stages[1][6], &bias, sizeof bias);
    RecompD3dPresenterDrawCommand draw = {0};
    draw.fvf = 0x142u;
    attach_combiner(device, &draw);
    REQUIRE(draw.has_combiner && memcmp(draw.combiner, words, sizeof words) == 0);
    REQUIRE(draw.reflection_bytes != NULL && draw.combiner_address_u == 3 && draw.combiner_address_v == 2);
    REQUIRE(draw.combiner_lod_bias == -2.0f);
    REQUIRE(!draw.combiner_is_backbuffer);
    *recomp_memory_u32(device + D3D_BACK_BUFFER_OFFSET) = resource;
    draw.has_combiner = false;
    attach_combiner(device, &draw);
    REQUIRE(draw.has_combiner && draw.combiner_is_backbuffer);
    *recomp_memory_u32(device + D3D_BACK_BUFFER_OFFSET) = 0;
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
        {0,11,1}, {1,11,1}, {1,3,1}, {1,4,1}, {1,4,3}, {1,5,1},
        {1,7,1}, {1,9,1}, {1,10,0x20000000u},
        {2,12,2}, {1,12,25}, {0,14,5}, {0,14,0x40}
    };
    for (uint32_t i = 0; i < sizeof rejected / sizeof rejected[0]; ++i) {
        uint32_t *word = &stages[rejected[i][0]][rejected[i][1]], old = *word;
        *word = rejected[i][2]; draw.has_combiner = false;
        attach_combiner(device, &draw);
        const bool rejected_draw = !draw.has_combiner;
        *word = old;
        REQUIRE(rejected_draw);
    }
    stages[1][29] = 0xff123456u;
    for (uint32_t axis = 0u; axis < 2u; ++axis) {
        const uint32_t old = stages[1][axis];
        stages[1][axis] = 4; draw.has_combiner = false;
        attach_combiner(device, &draw);
        const bool rejected_draw = !draw.has_combiner;
        stages[1][axis] = old;
        REQUIRE(rejected_draw);
    }
    stages[1][29] = 0;
    const uint32_t formats[] = {0x00000624u, 0x00000630u};
    for (uint32_t i = 0u; i < sizeof formats / sizeof formats[0]; ++i) {
        *recomp_memory_u32(resource + 12u) = formats[i]; draw.has_combiner = false;
        attach_combiner(device, &draw);
        const bool rejected_draw = !draw.has_combiner;
        *recomp_memory_u32(resource + 12u) = 0x00000620u;
        REQUIRE(rejected_draw);
    }
    stages[0][14] = 4;
    draw.fvf = 0x1c2u; draw.has_combiner = false;
    attach_combiner(device, &draw);
    REQUIRE(!draw.has_combiner);
    draw.fvf = 0x142u;
    attach_combiner(device, &draw);
    REQUIRE(draw.has_combiner);
    // Stage zero consumes TEXTURE only through its selected arguments.
    stages[0][14] = 2;
    stages[0][3] = stages[0][4] = stages[0][5] = 2;
    draw.has_combiner = false;
    attach_combiner(device, &draw);
    REQUIRE(draw.has_combiner);
    const uint32_t unsupported[] = {7u, 9u, 10u};
    for (uint32_t i = 0u; i < sizeof unsupported / sizeof unsupported[0]; ++i) {
        stages[0][unsupported[i]] = 1; draw.has_combiner = false;
        attach_combiner(device, &draw);
        const bool rejected_draw = !draw.has_combiner;
        stages[0][unsupported[i]] = 0;
        REQUIRE(rejected_draw);
    }
    stages[0][0] = 4; stages[0][29] = 0xff123456u; draw.has_combiner = false;
    attach_combiner(device, &draw);
    REQUIRE(!draw.has_combiner);
    stages[0][0] = stages[0][29] = 0;
    for (uint32_t index = 3u; index <= 5u; ++index) {
        for (uint32_t filter = 1u; filter <= 3u; filter += 2u) {
            stages[0][index] = filter; draw.has_combiner = false;
            attach_combiner(device, &draw);
            const bool rejected_draw = !draw.has_combiner;
            stages[0][index] = 2;
            REQUIRE(rejected_draw);
        }
    }
    for (uint32_t index = 21u; index <= 28u; index += 7u) {
        stages[0][index] = 1; draw.has_combiner = false;
        attach_combiner(device, &draw);
        const bool rejected_draw = !draw.has_combiner;
        stages[0][index] = 0;
        REQUIRE(rejected_draw);
    }
    stages[0][14] = 0;
    // No stage-one texture binding: ADD(CURRENT, DIFFUSE) still runs.
    recomp_runtime.registers.esp = 0x001f6500u;
    *recomp_memory_u32(0x001f6508u) = 0;
    set_texture();
    stages[1][12] = 7; stages[1][14] = 1; stages[1][15] = 0;
    stages[1][16] = 2; stages[1][18] = 1;
    stages[1][13] = stages[1][19] = 2; // Unused TEXTURE arguments.
    draw.has_combiner = false;
    attach_combiner(device, &draw);
    REQUIRE(draw.has_combiner && draw.reflection_bytes == NULL);
    for (uint32_t op = 14u; op <= 16u; op += 2u) {
        stages[1][12] = op; draw.has_combiner = false;
        attach_combiner(device, &draw);
        REQUIRE(!draw.has_combiner); // Implicit texture-alpha consumption.
    }
    passed = 1;
cleanup:
    recomp_d3d_texture_adapter_reset();
    recomp_runtime_init(NULL, 0u, NULL, 0u, NULL, 0u);
    return passed;
}
