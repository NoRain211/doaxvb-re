#ifndef DOAXBV_RECOMP_D3D_TEXTURE_MODEL_H
#define DOAXBV_RECOMP_D3D_TEXTURE_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RECOMP_D3D_TEXTURE_STAGE_COUNT = 4u,
};

enum {
    RECOMP_D3D_TEXTURE_CENSUS_SLOTS = 256u,
};

/* Block-compressed format bytes, from the guest's own format table. */
enum {
    RECOMP_D3D_TEXTURE_FORMAT_DXT1 = 0x0cu,
    RECOMP_D3D_TEXTURE_FORMAT_DXT3 = 0x0eu,
    RECOMP_D3D_TEXTURE_FORMAT_DXT5 = 0x0fu,
};

/* Uncompressed swizzled format bytes this presenter can upload. */
enum {
    RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8 = 0x06u,
    RECOMP_D3D_TEXTURE_FORMAT_P8 = 0x0bu,
    RECOMP_D3D_TEXTURE_FORMAT_A8 = 0x19u,
};

/* One decoded D3DResource. The guest stores dimensions two different ways
   and picks between them with Size, so both paths land here. */
typedef struct RecompD3dTextureDesc {
    uint32_t format_byte;
    uint32_t bits_per_pixel;
    bool render_target;
    bool depth;
    bool linear;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t data;
    uint32_t mip_levels;
} RecompD3dTextureDesc;

typedef struct RecompD3dTextureCensusEntry {
    bool used;
    RecompD3dTextureDesc desc;
    uint32_t stage;
    uint32_t bind_count;
} RecompD3dTextureCensusEntry;

typedef struct RecompD3dTextureCensus {
    RecompD3dTextureCensusEntry entries[RECOMP_D3D_TEXTURE_CENSUS_SLOTS];
    uint32_t overflow_count;
} RecompD3dTextureCensus;

typedef struct RecompD3dTextureModel {
    uint32_t textures[RECOMP_D3D_TEXTURE_STAGE_COUNT];
    uint32_t update_count;
    RecompD3dTextureCensus census;
} RecompD3dTextureModel;

void recomp_d3d_texture_reset(RecompD3dTextureModel *model);
bool recomp_d3d_set_texture(
    RecompD3dTextureModel *model,
    uint32_t stage,
    uint32_t texture);
bool recomp_d3d_texture_resolve_cpu_address(
    uint32_t locked_address,
    uint32_t *cpu_address);

/* `descriptor_byte` is the guest's own per-format descriptor, read from the
   table the D3D8 code indexes by format byte. Passing it in keeps this
   decode free of guest memory access. */
bool recomp_d3d_texture_describe(
    uint32_t format_dword,
    uint32_t size_dword,
    uint32_t data,
    uint32_t descriptor_byte,
    RecompD3dTextureDesc *out);
uint32_t recomp_d3d_texture_compressed_mip_span(
    const RecompD3dTextureDesc *desc);
/* Full static payload, including every uncompressed mip. Zero on invalid input. */
uint32_t recomp_d3d_texture_mip_span(const RecompD3dTextureDesc *desc);
void recomp_d3d_texture_census_record(
    RecompD3dTextureCensus *census,
    const RecompD3dTextureDesc *desc,
    uint32_t stage);

/* NV2A stores non-linear surfaces in Morton order: the low bits of x and y
   alternate, and once the smaller dimension runs out of bits the remaining
   bits of the larger one sit above the interleaved region. Both results are
   texel indices, so callers scale by bytes per texel. */
void recomp_d3d_texture_swizzle_masks(
    uint32_t width,
    uint32_t height,
    uint32_t *mask_x,
    uint32_t *mask_y);
uint32_t recomp_d3d_texture_swizzle_offset(
    uint32_t x,
    uint32_t y,
    uint32_t mask_x,
    uint32_t mask_y);
/* Rewrites a swizzled surface into row-major order. Returns false unless the
   sizes are sane and both buffers hold width * height * bytes_per_texel. */
bool recomp_d3d_texture_unswizzle(
    const uint8_t *source,
    uint8_t *destination,
    uint32_t width,
    uint32_t height,
    uint32_t bytes_per_texel);

#ifdef __cplusplus
}
#endif

#endif
