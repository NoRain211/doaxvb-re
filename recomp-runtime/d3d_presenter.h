#ifndef DOAXBV_RECOMP_D3D_PRESENTER_H
#define DOAXBV_RECOMP_D3D_PRESENTER_H

#include <stdbool.h>
#include <stdint.h>

#include "d3d_render_state_model.h"
#include "d3d_texture_model.h"

typedef struct RecompD3dPresenter RecompD3dPresenter;

typedef enum RecompD3dPresenterColorFormat {
    RECOMP_D3D_PRESENTER_COLOR_FORMAT_UNKNOWN,
    /* Xbox linear A8R8G8B8 (0x12) uses BGRA8 host storage. Clear colors
       remain logical RGBA values at the presenter seam. */
    RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
} RecompD3dPresenterColorFormat;

typedef enum RecompD3dPresenterDepthFormat {
    RECOMP_D3D_PRESENTER_DEPTH_FORMAT_UNKNOWN,
    /* Xbox linear D24S8 (0x2e). */
    RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8,
} RecompD3dPresenterDepthFormat;

typedef struct RecompD3dPresenterConfig {
    uint32_t width;
    uint32_t height;
    RecompD3dPresenterColorFormat color_format;
    RecompD3dPresenterDepthFormat depth_format;
} RecompD3dPresenterConfig;

typedef enum RecompD3dPresenterCommandType {
    RECOMP_D3D_PRESENTER_COMMAND_CLEAR,
    RECOMP_D3D_PRESENTER_COMMAND_PRESENT,
    RECOMP_D3D_PRESENTER_COMMAND_DRAW,
    RECOMP_D3D_PRESENTER_COMMAND_GAMMA,
} RecompD3dPresenterCommandType;

/* Resolved by the adapter. Surface wrappers sharing pixel storage have the
   same color descriptor; the backend never reads a guest device object. */
typedef struct RecompD3dPresenterTarget {
    bool offscreen;
    bool no_depth;
    bool custom_depth;
    RecompD3dTextureDesc color;
    /* Otherwise use the default host depth surface (unless no_depth). */
    RecompD3dTextureDesc depth;
} RecompD3dPresenterTarget;

typedef struct RecompD3dPresenterClearCommand {
    bool clear_color;
    bool clear_depth;
    bool clear_stencil;
    uint32_t color;
    float z;
    uint32_t stencil;
    RecompD3dPresenterTarget target;
} RecompD3dPresenterClearCommand;

typedef struct RecompD3dPresenterPresentCommand {
    uint32_t effective_flags;
    uint32_t swap_counter;
} RecompD3dPresenterPresentCommand;

/* Directional and point diffuse/ambient lighting for the material-source path. */
typedef struct RecompD3dDirectionalLighting {
    bool enabled;
    bool normalize;
    uint32_t count;
    float normal_transforms[4][16];
    float ambient_emissive[4];
    float material_diffuse[4];
    float directions[8][4];
    float colors[8][4];
    float world_transforms[4][16];
    /* World position xyz; w is 1 for point lights, 0 for directional. */
    float positions[8][4];
    /* Constant, linear, quadratic attenuation, and range. */
    float attenuation[8][4];
    /* Point-light ambient multiplied by material ambient, before attenuation. */
    float ambient[8][4];
} RecompD3dDirectionalLighting;

typedef enum RecompD3dCullMode {
    RECOMP_D3D_CULL_NONE,
    RECOMP_D3D_CULL_CLOCKWISE,
    RECOMP_D3D_CULL_COUNTER_CLOCKWISE,
} RecompD3dCullMode;

/* One indexed draw. Buffer contents stay in guest memory: the adapter passes
   host pointers and byte counts it has already bounds-checked, so the
   presenter never decodes guest addresses itself. */
typedef struct RecompD3dPresenterDrawCommand {
    RecompD3dCullMode cull_mode;
    uint32_t primitive_type;
    uint32_t index_count;
    uint32_t triangle_count;
    uint32_t vertex_count;
    uint32_t vertex_stride;
    uint32_t fvf;
    /* Bounded API-level vertex program; zero count selects fixed function. */
    uint32_t program_count;
    uint32_t program[136][4];
    float program_constants[192][4];
    bool program_alpha_mask;
    float program_mask_lod_bias;
    RecompD3dDirectionalLighting directional;
    const void *vertex_bytes;
    const void *index_bytes;
    /* World-view-projection rows, already composed by the adapter. */
    float transform[16];
    /* Additional world-view-projection matrices for 1..3 explicit weights. */
    float blend_transforms[3][16];
    uint32_t blend_weight_count;
    bool has_transform;
    /* Depth, stencil, and alpha-test state for this draw, already decoded by
       the render-state model so the presenter never sees a method number. */
    RecompD3dDepthState depth;
    RecompD3dFogState fog;
    bool fog_z; /* Affine projection uses Z; perspective uses clip W. */
    float fog_world_view[4][16]; /* Only needed for range fog. */
    RecompD3dBlendState blend;
    /* Stage 0 selects this ARGB factor for both color and alpha. */
    bool use_texture_factor;
    /* Stage 1 multiplies the sampled result by this factor. */
    bool modulate_texture_factor;
    uint32_t texture_factor;
    /* Measured stage-0 selection or modulation of material alpha. */
    RecompD3dMaterialAlphaMode material_alpha_mode;
    float material_alpha;
    /* ponytail: exact zero illumination; full lighting needs light evaluation. */
    bool zero_diffuse_rgb;
    /* Exact four-source register-combiner filter; uses all four UV sets. */
    bool four_tap_filter;
    /* Stage 0 texture for this draw. `texture_bytes` is a host-readable view
       of guest pixel memory, valid only for the duration of the submit. */
    RecompD3dTextureDesc texture;
    bool has_texture;
    /* Guest D3DTADDRESS U/V for the texture's stage; 0 (unknown) wraps. */
    uint32_t address_u, address_v;
    /* Storage aliases the current guest backbuffer, whose pixels are host-owned. */
    bool texture_is_backbuffer;
    const void *texture_bytes;
    uint32_t texture_byte_count;
    /* Bound P8 palette in guest ARGB32 order, valid during the submit. */
    const void *palette_bytes;
    uint32_t palette_byte_count;
    /* Separate stage-0 alpha mask; stage-1 color uses the second UV set. */
    bool has_alpha_mask;
    RecompD3dTextureDesc alpha_mask;
    const void *alpha_mask_bytes;
    uint32_t alpha_mask_byte_count;
    const void *alpha_mask_palette;
    uint32_t alpha_mask_palette_byte_count;
    /* Fixed-function texture-alpha reflection blend, followed by diffuse modulation. */
    bool has_reflection;
    bool reflection_normalize;
    bool reflection_mesh_uv;
    RecompD3dTextureDesc reflection_texture;
    const void *reflection_bytes;
    uint32_t reflection_byte_count;
    float reflection_world_view[16];
    float reflection_normal[16];
    float reflection_transform[16];
    float reflection_diffuse[4];
    RecompD3dPresenterTarget target;
    /* Optional immutable RecompD3dPoseReplay, copied during submit. */
    const void *pose_replay;
    /* Optional contiguous diagnostic vertex samples, owned with the packet. */
    const void *pose_vertex_bytes;
    /* Immutable RecompSplitDraw followed by optional seam vertex inputs. */
    const void *split_pose;
    uint32_t split_pose_size;
} RecompD3dPresenterDrawCommand;

typedef struct RecompD3dPresenterCommand {
    RecompD3dPresenterCommandType type;
    union {
        RecompD3dPresenterClearCommand clear;
        RecompD3dPresenterPresentCommand present;
        RecompD3dPresenterDrawCommand draw;
        /* Xbox gamma ramp: separate 256-entry 8-bit red, green, blue planes. */
        uint8_t gamma[3][256];
    } data;
} RecompD3dPresenterCommand;

typedef enum RecompD3dPresenterError {
    RECOMP_D3D_PRESENTER_OK,
    RECOMP_D3D_PRESENTER_INVALID_ARGUMENT,
    RECOMP_D3D_PRESENTER_NOT_INITIALIZED,
    RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED,
    RECOMP_D3D_PRESENTER_OUT_OF_MEMORY,
    RECOMP_D3D_PRESENTER_HOST_FAILURE,
    RECOMP_D3D_PRESENTER_WRONG_THREAD,
    RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND,
    RECOMP_D3D_PRESENTER_COMMAND_LIMIT,
    RECOMP_D3D_PRESENTER_CLOSED,
} RecompD3dPresenterError;

/* Lifecycle calls and submissions occur on one owning thread. Create requires
   a null output handle, and destroy releases all adapter state and nulls the
   handle. The D3D11 build copies each submit and renders it on a worker thread
   at the next PRESENT. A failed draw is counted as a decline, as in the
   synchronous path; other errors surface on a later call or on destroy. */
#ifdef __cplusplus
extern "C" {
#endif

RecompD3dPresenterError recomp_d3d_presenter_create(
    const RecompD3dPresenterConfig *config,
    RecompD3dPresenter **presenter);
RecompD3dPresenterError recomp_d3d_presenter_submit(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterCommand *command);
RecompD3dPresenterError recomp_d3d_presenter_destroy(
    RecompD3dPresenter **presenter);

/* Retire pixels only when their backing allocation is released, not when a
   nonowning surface wrapper is destroyed. Accepts guest RAM aliases. */
RecompD3dPresenterError recomp_d3d_presenter_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size);

/* Process-wide host pacing toggle. When enabled, presents use
   DXGI_PRESENT_DO_NOT_WAIT instead of vsync so the guest frame loop runs at
   CPU speed. The guest does not depend on wall-clock time (kernel waits are
   already immediate), so this only removes unintended host pacing; it does
   not force guest state. Intended for the full-program runner; the default
   (vsync) remains for windowed tests. */
void recomp_d3d_presenter_set_immediate_present(bool enabled);
/* Resolved split state after presenter creation; false before initialization. */
bool recomp_d3d_presenter_split_enabled(void);

/* Observation only: reports which guest texture formats draws actually
   sampled, and which ones a draw asked for but the presenter could not
   upload. Bind counts alone cannot answer that. */
void recomp_d3d_presenter_report_draw_textures(void);

#ifdef __cplusplus
}
#endif

#endif
