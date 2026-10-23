#ifndef DOAXBV_RECOMP_D3D_DRAW_MODEL_H
#define DOAXBV_RECOMP_D3D_DRAW_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Xbox primitive types. These are the NV2A SET_BEGIN_END values, not the PC
   D3DPRIMITIVETYPE ones: D3DDevice_DrawIndexedVertices at 0x001E78B0 writes
   the caller's primitive straight into method 0x17FC without translating it,
   so the guest is already speaking the hardware encoding. The PC enum is one
   lower across the board, which silently turns strips into fans. */
enum {
    RECOMP_D3D_PT_LINELIST = 2u,
    RECOMP_D3D_PT_TRIANGLELIST = 5u,
    RECOMP_D3D_PT_TRIANGLESTRIP = 6u,
    RECOMP_D3D_PT_TRIANGLEFAN = 7u,
};

typedef enum RecompD3dDrawError {
    RECOMP_D3D_DRAW_OK,
    RECOMP_D3D_DRAW_INVALID_ARGUMENT,
    RECOMP_D3D_DRAW_NO_STREAM,
    RECOMP_D3D_DRAW_UNSUPPORTED_PRIMITIVE,
    RECOMP_D3D_DRAW_EMPTY,
} RecompD3dDrawError;

/* Vertex stream binding, as set by D3DDevice_SetStreamSource. */
typedef struct RecompD3dStream {
    uint32_t vertex_data;
    uint32_t stride;
} RecompD3dStream;

typedef struct RecompD3dDrawState {
    RecompD3dStream stream0;
    uint32_t fvf;
} RecompD3dDrawState;

/* A validated draw, still expressed in guest addresses. Resolving those to
   host pointers is the adapter's job, which keeps this model free of any
   knowledge of how the runtime maps guest memory. */
typedef struct RecompD3dDrawPlan {
    uint32_t primitive_type;
    uint32_t index_count;
    uint32_t triangle_count;
    uint32_t index_data;
    uint32_t index_bytes;
    uint32_t vertex_data;
    uint32_t vertex_stride;
    uint32_t fvf;
} RecompD3dDrawPlan;

typedef struct RecompD3dDrawResult {
    RecompD3dDrawError error;
    RecompD3dDrawPlan plan;
} RecompD3dDrawResult;

bool recomp_d3d_normal_transform(const float world[16], float normal[16]);

void recomp_d3d_draw_reset(RecompD3dDrawState *state);

/* Translates a guest DrawIndexedVertices into a validated draw plan. Pure:
   it reads only the state handed to it and never touches guest memory, so it
   is unit-testable without a runtime. */
RecompD3dDrawResult recomp_d3d_draw_indexed(
    const RecompD3dDrawState *state,
    uint32_t primitive_type,
    uint32_t index_count,
    uint32_t index_data);

/* Number of triangles a primitive count yields, or 0 when unsupported. */
uint32_t recomp_d3d_draw_triangle_count(
    uint32_t primitive_type,
    uint32_t index_count);

/* Byte size of the vertex range an index buffer reaches into, given the
   largest index it contains. Returns 0 when the span would overflow. */
uint32_t recomp_d3d_draw_vertex_bytes(uint32_t stride, uint32_t max_index);

/* Vertex size implied by a fixed-function FVF, or 0 when the FVF uses a
   component this seam does not decode yet. FVF 0x112 (XYZ|NORMAL|TEX1)
   yields 32, matching the stride the guest binds. */
uint32_t recomp_d3d_fvf_stride(uint32_t fvf);

/* Returns the fixed-function FVF the host should decode for a measured guest
   stream: trailing texture-coordinate sets are dropped until the layout fits
   the bound stride. The result still exceeds the stride when nothing fits. */
uint32_t recomp_d3d_fvf_for_stream(uint32_t fvf, uint32_t vertex_stride);

/* Byte offset of one FVF component within a vertex, or
   RECOMP_D3D_FVF_ABSENT when the FVF does not carry it. Components are laid
   out in a fixed order - position, blend weights, normal, diffuse, specular,
   then texture coordinates - so a component's offset depends on which earlier
   components are present. A consumer that assumes fixed offsets reads the
   wrong bytes for any FVF whose earlier components differ. */
enum { RECOMP_D3D_FVF_ABSENT = 0xffffffffu };

typedef struct RecompD3dVertexLayout {
    uint32_t stride;
    uint32_t position_offset;
    bool pretransformed; /* Position is screen XYZ followed by reciprocal W. */
    uint32_t blend_weight_count; /* Explicit float weights start at byte 12. */
    uint32_t normal_offset;
    uint32_t diffuse_offset;
    uint32_t specular_offset;
    uint32_t texcoord_offset;
    uint32_t texcoord_count;
} RecompD3dVertexLayout;

/* Decodes an FVF into component offsets. Returns false, leaving the layout
   untouched, when the FVF uses a component this seam does not decode. */
bool recomp_d3d_fvf_layout(uint32_t fvf, RecompD3dVertexLayout *layout);

#ifdef __cplusplus
}
#endif

#endif
