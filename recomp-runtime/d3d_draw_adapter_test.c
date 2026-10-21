#include "d3d_draw_adapter.c"
#include "d3d_presenter_memory_test.h"

#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "draw adapter line %d\n", __LINE__); goto cleanup; } } while (0)

static int combiner_adapter_test(void)
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
    REQUIRE(!draw.combiner_is_backbuffer && !draw.combiner_mip_point);
    // The title sea samples its shimmer with a point mip filter.
    stages[1][5] = 1; draw.has_combiner = false;
    attach_combiner(device, &draw);
    REQUIRE(draw.has_combiner && draw.combiner_mip_point);
    stages[1][5] = 2;
    *recomp_memory_u32(device + D3D_BACK_BUFFER_OFFSET) = resource;
    draw.has_combiner = false;
    attach_combiner(device, &draw);
    REQUIRE(draw.has_combiner && draw.combiner_is_backbuffer);
    *recomp_memory_u32(device + D3D_BACK_BUFFER_OFFSET) = 0;
    *recomp_memory_u32(device + D3D_BACK_BUFFER_OFFSET + 4u) = resource;
    draw.has_combiner = false;
    attach_combiner(device, &draw);
    REQUIRE(draw.has_combiner && draw.combiner_is_frontbuffer && !draw.combiner_is_backbuffer);
    *recomp_memory_u32(device + D3D_BACK_BUFFER_OFFSET + 4u) = 0;
    // Unmapped stage-one pixels decline to the stage-zero path.
    *recomp_memory_u32(resource + 4u) = 0x00900000u;
    recomp_runtime.registers.esp = 0x001f6500u;
    set_texture();
    draw.has_combiner = false;
    attach_combiner(device, &draw);
    REQUIRE(!draw.has_combiner);
    // Unless they alias the presented front buffer.
    *recomp_memory_u32(device + D3D_BACK_BUFFER_OFFSET + 4u) = resource;
    attach_combiner(device, &draw);
    REQUIRE(draw.has_combiner && draw.combiner_is_frontbuffer && draw.reflection_bytes == NULL);
    *recomp_memory_u32(device + D3D_BACK_BUFFER_OFFSET + 4u) = 0;
    *recomp_memory_u32(resource + 4u) = resource + 0x100u;
    // A 2x2 two-level A8R8G8B8 chain hands the presenter every mip.
    *recomp_memory_u32(resource + 12u) = 0x01120620u;
    recomp_runtime.registers.esp = 0x001f6500u;
    set_texture();
    draw.has_combiner = false;
    attach_combiner(device, &draw);
    REQUIRE(draw.has_combiner && draw.reflection_texture.mip_levels == 2u &&
        draw.reflection_byte_count == 20u);
    *recomp_memory_u32(resource + 12u) = 0x00000620u;
    recomp_runtime.registers.esp = 0x001f6500u;
    set_texture();
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
        {0,11,1}, {1,11,1}, {1,3,1}, {1,4,1}, {1,4,3}, {1,5,0}, {1,5,3},
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
    memcpy(&stages[0][6], &bias, sizeof bias);
    draw.has_combiner = false;
    attach_combiner(device, &draw);
    const bool biased_rejected = !draw.has_combiner;
    stages[0][6] = 0;
    REQUIRE(biased_rejected);
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


static int specular_adapter_test(void)
{
    int passed = 0;
    uint8_t memory[0x7000] = {0};
    const RecompMemoryRegion region = {0x001f0000u, sizeof memory, memory};
    const uint32_t device = 0x001f3120u, light = 0x001f6100u;
    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    *recomp_memory_u32(0x001f2d20u) = 1;
    *recomp_memory_u32(0x001f2d24u) = 1;
    *recomp_memory_u32(0x001f29b8u) = 4;
    *recomp_memory_u32(0x001f29c0u) = 2;
    *recomp_memory_u32(device + 0x398u) = light;
    *recomp_memory_u32(light) = 3;
    float *material = (float *)(void *)recomp_memory_u32(device + 0xab0u);
    float *data = (float *)(void *)recomp_memory_u32(light);
    for (unsigned c = 0; c < 3; ++c) {
        material[c] = data[1+c] = 1;
        material[8+c] = 0.5f;
        data[5+c] = 0.25f;
    }
    material[16] = 10;
    data[18] = -1;
    float *world = (float *)(void *)recomp_memory_u32(device + 0x990u);
    float *view = (float *)(void *)recomp_memory_u32(device + 0x810u);
    for (unsigned i = 0; i < 4; ++i) world[i*5] = view[i*5] = 1;
    RecompD3dPresenterDrawCommand draw = {0};
    draw.fvf = 0x112;
    attach_directional_lighting(device, &draw);
    REQUIRE(draw.directional.enabled && draw.directional.view[2] == -1);
    REQUIRE(draw.directional.view[3] == 10 && draw.directional.specular[0][0] == 0.125f);
    view[0] = view[10] = 0; view[2] = 2; view[8] = -2;
    attach_directional_lighting(device, &draw);
    REQUIRE(draw.directional.view[0] == -1 && draw.directional.view[2] == 0);
    material[16] = 0;
    attach_directional_lighting(device, &draw);
    REQUIRE(draw.directional.view[3] > 0);
    *recomp_memory_u32(0x001f2d28u) = 1;
    attach_directional_lighting(device, &draw);
    REQUIRE(draw.directional.enabled && draw.directional.view[3] == 0);
    *recomp_memory_u32(0x001f2d28u) = 0;
    *recomp_memory_u32(0x001f2d24u) = 0;
    data[5] = NAN;
    attach_directional_lighting(device, &draw);
    REQUIRE(draw.directional.enabled && draw.directional.specular[0][0] == 0);
    *recomp_memory_u32(0x001f2d24u) = 1;
    draw.directional.enabled = false;
    attach_directional_lighting(device, &draw);
    REQUIRE(!draw.directional.enabled);
    data[5] = 0.25f; material[16] = NAN;
    attach_directional_lighting(device, &draw);
    REQUIRE(!draw.directional.enabled);
    passed = 1;
cleanup:
    recomp_runtime_init(NULL, 0u, NULL, 0u, NULL, 0u);
    return passed;
}

static int line_list_adapter_test(void)
{
    int passed = 0;
    uint8_t memory[0x7000] = {0};
    const RecompMemoryRegion region = {0x001f0000u, sizeof memory, memory};
    const uint32_t device = 0x001f3120u, vertices = 0x001f6000u, esp = 0x001f6500u;
    const RecompD3dPresenterConfig config = {4, 4,
        RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM, RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8};
    RecompD3dPresenterMemorySnapshot snapshot;
    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    recomp_d3d_frame_adapter_initialize(&config, device);
    *recomp_memory_u32(D3D_DEVICE_GLOBAL) = device;
    *recomp_memory_u32(device + 0x21b4u) = vertices;
    *recomp_memory_u32(device + 0x21c0u) = vertices;
    for (unsigned transform = 0; transform <= 6; ++transform) {
        float *matrix = (float *)(void *)recomp_memory_u32(device + 0x810u + transform*64u);
        for (unsigned i = 0; i < 4; ++i) matrix[i*5] = 1;
    }
    const struct { uint32_t fvf, stride, count; bool valid; } cases[] = {
        {0x42,16,2,true}, {0x44,20,8,true}, {0x44,20,3,false},
        {0x44,20,0,false}, {0x44,20,65538,false}, {0x104,24,2,false},
        {0x44,20,2,false}, {0x42,16,2,false}, {0x44,20,2,false},
    };
    unsigned submitted = 0;
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        *recomp_memory_u32(device + 0x384u) = cases[c].fvf;
        for (unsigned i = 0; i < 8; ++i) {
            float position[] = {0, 0, 0.25f, 1};
            memcpy(recomp_memory_u32(vertices + i*cases[c].stride), position, cases[c].fvf == 0x42 ? 12 : 16);
            *recomp_memory_u32(vertices + i*cases[c].stride + cases[c].stride - 4) = 0xffffffffu;
        }
        if (c == 6) *recomp_memory_u32(vertices + 12) = 0; // Invalid RHW.
        if (c == 7) *(float *)(void *)recomp_memory_u32(vertices) = NAN;
        uint32_t args[] = {0, RECOMP_D3D_PT_LINELIST, cases[c].count,
            c == 8 ? 0x00900000u : vertices, cases[c].stride};
        memcpy(recomp_memory_u32(esp), args, sizeof args);
        recomp_runtime.registers.esp = esp;
        recomp_d3d_draw_vertices_up_adapter();
        REQUIRE(recomp_runtime.registers.esp == esp + 20);
        submitted += cases[c].valid;
        REQUIRE(recomp_d3d_presenter_memory_snapshot(&snapshot) && snapshot.draw_count == submitted);
        if (cases[c].valid) {
            const RecompD3dPresenterDrawCommand *draw = &snapshot.commands[submitted-1].data.draw;
            REQUIRE(draw->primitive_type == RECOMP_D3D_PT_LINELIST && draw->triangle_count == 0);
            REQUIRE(draw->index_count == cases[c].count && draw->fvf == cases[c].fvf);
            REQUIRE(draw->has_transform == (cases[c].fvf == 0x42));
        }
    }
    *recomp_memory_u32(0x001f2e20u) = 20;
    *recomp_memory_u32(0x001f2e28u) = 0x001f6300u;
    *recomp_memory_u32(0x001f6304u) = vertices;
    const uint16_t indices[] = {0,1};
    memcpy(recomp_memory_u32(0x001f6400u), indices, sizeof indices);
    const uint32_t indexed_args[] = {0,RECOMP_D3D_PT_LINELIST,2,0x001f6400u};
    memcpy(recomp_memory_u32(esp), indexed_args, sizeof indexed_args);
    recomp_runtime.registers.esp = esp;
    recomp_d3d_draw_indexed_vertices_adapter();
    REQUIRE(recomp_runtime.registers.esp == esp + 16);
    REQUIRE(recomp_d3d_presenter_memory_snapshot(&snapshot) && snapshot.draw_count == submitted + 1);
    REQUIRE(snapshot.commands[submitted].data.draw.primitive_type == RECOMP_D3D_PT_LINELIST);
    passed = 1;
cleanup:
    recomp_d3d_frame_adapter_reset();
    recomp_runtime_init(NULL, 0u, NULL, 0u, NULL, 0u);
    return passed;
}

#undef REQUIRE
#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "draw adapter line %d\n", __LINE__); return 0; } } while (0)

static int backbuffer_adapter_test(void)
{
    uint8_t memory[0x100] = {0};
    uint32_t slots[2] = {0x1000u, 0x1020u};
    uint8_t format = 0x20;
    const uint32_t device = 0x2000u;
    RecompMemoryRegion regions[] = {
        {0x1000u, sizeof memory, memory},
        {device + D3D_BACK_BUFFER_OFFSET, sizeof slots, (uint8_t *)slots},
        {0x001f16b8u + 6u, 1u, &format}
    };
    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t words[5] = {0, 0x3000u + i * 0x100u, 0, 0x00600620u, 0};
        memcpy(memory + i * 0x20u, words, sizeof words);
    }
    recomp_runtime_init(regions, 3u, NULL, 0u, NULL, 0u);
    for (uint32_t front = 0; front < 2; ++front) {
        RecompD3dPresenterDrawCommand draw = {0};
        draw.has_texture = true;
        REQUIRE(recomp_d3d_texture_adapter_describe(slots[front], &draw.texture));
        attach_backbuffer_texture(device, &draw);
        REQUIRE(draw.texture_is_frontbuffer == (front != 0));
        REQUIRE(draw.texture_is_backbuffer == (front == 0));
    }
    // Map only the front slot: an unmapped back slot must not end the search.
    regions[1].address += 4u; regions[1].size = 4u; regions[1].data += 4u;
    recomp_runtime_init(regions, 3u, NULL, 0u, NULL, 0u);
    RecompD3dPresenterDrawCommand draw = {0};
    draw.has_texture = true;
    REQUIRE(recomp_d3d_texture_adapter_describe(slots[1], &draw.texture));
    attach_backbuffer_texture(device, &draw);
    REQUIRE(draw.texture_is_frontbuffer && !draw.texture_is_backbuffer);
    recomp_runtime_init(NULL, 0u, NULL, 0u, NULL, 0u);
    return 1;
}

int recomp_d3d_draw_adapter_test(void)
{
    return combiner_adapter_test() && backbuffer_adapter_test() &&
        specular_adapter_test() && line_list_adapter_test();
}
