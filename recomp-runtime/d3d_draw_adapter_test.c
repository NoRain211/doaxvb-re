#include "d3d_draw_adapter.c"

#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "draw adapter line %d\n", __LINE__); return 0; } } while (0)

int recomp_d3d_draw_adapter_test(void)
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
