// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_presenter.h"
#include "d3d_draw_model.h"
#include "d3d_texture_model.h"
#include "d3d_vertex_program.h"

#include <SDL3/SDL.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t kTextureSlots = 4096u;
constexpr uint32_t kPaletteBytes = 256u * 4u;
constexpr uint64_t kTargetByteLimit = 64u * 1024u * 1024u;
constexpr uint32_t kInitialVertexCapacity = 32u * 1024u * 1024u; // 32 MB
constexpr uint32_t kInitialIndexCapacity = 16u * 1024u * 1024u;  // 16 MB
// TransformUniforms in floats: the header every draw pushes, then the
// directional block. SDL's Vulkan backend binds 4 KB per uniform slot, so
// vertex program constants live in their own slot.
constexpr uint32_t kHeaderUniformFloats = 144u;
constexpr uint32_t kTransformUniformFloats = 280u;
constexpr uint32_t kProgramConstantFloats = 192u * 4u;
constexpr uint32_t kNoProgramConstants = UINT32_MAX;

constexpr uint32_t kBootDrawFvfs[] = {
    0x042u, 0x104u, 0x112u, 0x116u, 0x118u, 0x11Au,
    0x142u, 0x144u, 0x212u, 0x216u, 0x21Au, 0x242u, 0x244u, 0x404u
};

constexpr char kMslShaderPrologue[] = R"(#include <metal_stdlib>
using namespace metal;

struct TransformUniforms {
    float4x4 wvp[4];
    float4 draw_flags;
    float4 blend_flags;
    float4 texture_factor;
    float4 lighting_flags;
    float4 texture_flags;
    float4x4 reflection_world_view;
    float4x4 reflection_normal;
    float4x4 reflection_transform;
    float4 reflection_diffuse;
    float4 reflection_flags;
    float4 directional_flags;
    float4x4 directional_normals[4];
    float4 directional_base;
    float4 directional_material;
    float4 directional_directions[8];
    float4 directional_colors[8];
};

struct ProgramConstants {
    float4 vc[192];
};
)";

struct TextureEntry {
    bool used = false;
    uint32_t data = 0;
    uint32_t format_byte = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 0;
    uint8_t palette[kPaletteBytes]{};
    uint64_t fingerprint = 0;
    SDL_GPUTexture *texture = nullptr;
};

struct RenderTargetEntry {
    RecompD3dTextureDesc desc{};
    SDL_GPUTexture *texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct DepthTargetEntry {
    RecompD3dTextureDesc desc{};
    SDL_GPUTexture *texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

SDL_GPUBlendFactor hostBlendFactor(RecompD3dBlendFactor factor, bool alpha_channel)
{
    switch (factor) {
    case RECOMP_D3D_BLEND_ZERO: return SDL_GPU_BLENDFACTOR_ZERO;
    case RECOMP_D3D_BLEND_ONE: return SDL_GPU_BLENDFACTOR_ONE;
    case RECOMP_D3D_BLEND_SRC_COLOR:
        return alpha_channel ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case RECOMP_D3D_BLEND_INV_SRC_COLOR:
        return alpha_channel ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case RECOMP_D3D_BLEND_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case RECOMP_D3D_BLEND_INV_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case RECOMP_D3D_BLEND_DST_ALPHA: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case RECOMP_D3D_BLEND_INV_DST_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case RECOMP_D3D_BLEND_DST_COLOR:
        return alpha_channel ? SDL_GPU_BLENDFACTOR_DST_ALPHA : SDL_GPU_BLENDFACTOR_DST_COLOR;
    case RECOMP_D3D_BLEND_INV_DST_COLOR:
        return alpha_channel ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA : SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case RECOMP_D3D_BLEND_SRC_ALPHA_SATURATE: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case RECOMP_D3D_BLEND_CONSTANT_COLOR: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    case RECOMP_D3D_BLEND_INV_CONSTANT_COLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: return SDL_GPU_BLENDFACTOR_ONE;
    }
}

SDL_GPUBlendOp hostBlendOp(RecompD3dBlendOp op)
{
    switch (op) {
    case RECOMP_D3D_BLEND_OP_MIN: return SDL_GPU_BLENDOP_MIN;
    case RECOMP_D3D_BLEND_OP_MAX: return SDL_GPU_BLENDOP_MAX;
    case RECOMP_D3D_BLEND_OP_SUBTRACT: return SDL_GPU_BLENDOP_SUBTRACT;
    case RECOMP_D3D_BLEND_OP_REVERSE_SUBTRACT: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case RECOMP_D3D_BLEND_OP_ADD:
    default: return SDL_GPU_BLENDOP_ADD;
    }
}

SDL_GPUCompareOp hostCompareOp(RecompD3dCompareFunc func)
{
    switch (func) {
    case RECOMP_D3D_COMPARE_NEVER: return SDL_GPU_COMPAREOP_NEVER;
    case RECOMP_D3D_COMPARE_LESS: return SDL_GPU_COMPAREOP_LESS;
    case RECOMP_D3D_COMPARE_EQUAL: return SDL_GPU_COMPAREOP_EQUAL;
    case RECOMP_D3D_COMPARE_LESS_EQUAL: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case RECOMP_D3D_COMPARE_GREATER: return SDL_GPU_COMPAREOP_GREATER;
    case RECOMP_D3D_COMPARE_NOT_EQUAL: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case RECOMP_D3D_COMPARE_GREATER_EQUAL: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    case RECOMP_D3D_COMPARE_ALWAYS: return SDL_GPU_COMPAREOP_ALWAYS;
    default: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    }
}

SDL_GPUStencilOp hostStencilOp(RecompD3dStencilOp op)
{
    switch (op) {
    case RECOMP_D3D_STENCIL_KEEP: return SDL_GPU_STENCILOP_KEEP;
    case RECOMP_D3D_STENCIL_ZERO: return SDL_GPU_STENCILOP_ZERO;
    case RECOMP_D3D_STENCIL_REPLACE: return SDL_GPU_STENCILOP_REPLACE;
    case RECOMP_D3D_STENCIL_INCRSAT: return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
    case RECOMP_D3D_STENCIL_DECRSAT: return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
    case RECOMP_D3D_STENCIL_INVERT: return SDL_GPU_STENCILOP_INVERT;
    case RECOMP_D3D_STENCIL_INCRWRAP: return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
    case RECOMP_D3D_STENCIL_DECRWRAP: return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP;
    default: return SDL_GPU_STENCILOP_KEEP;
    }
}

SDL_GPUSamplerAddressMode hostAddressMode(uint32_t mode)
{
    switch (mode) {
    case 1: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;          // WRAP
    case 2: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT; // MIRROR
    case 3: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;    // CLAMP
    case 4: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;    // BORDER
    case 5: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;    // CLAMPTOEDGE
    default: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    }
}

uint64_t textureFingerprint(const void *bytes, uint32_t count)
{
    const auto *data = static_cast<const uint8_t *>(bytes);
    uint64_t hash = 0xcbf29ce484222325ull ^ count;
    if (count < 8u) {
        for (uint32_t i = 0u; i < count; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
        return hash;
    }
    constexpr uint32_t kSamples = 128u;
    for (uint32_t i = 0u; i < kSamples; ++i) {
        uint64_t word;
        std::memcpy(&word, data + static_cast<uint64_t>(count - 8u) * i / (kSamples - 1u), 8u);
        hash = (hash ^ word) * 0x100000001b3ull;
    }
    return hash;
}

// Vertex programs key shaders and pipelines, so every token counts; the
// sampled texture fingerprint would let two programs share one shader.
uint64_t programHash(const uint32_t (*tokens)[4], uint32_t count)
{
    const auto *data = reinterpret_cast<const uint8_t *>(tokens);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t i = 0u; i < count * 16u; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}

SDL_GPUTextureFormat hostTextureFormat(uint32_t format_byte)
{
    switch (format_byte) {
    case RECOMP_D3D_TEXTURE_FORMAT_DXT1: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    case RECOMP_D3D_TEXTURE_FORMAT_DXT3: return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
    case RECOMP_D3D_TEXTURE_FORMAT_DXT5: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    case RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8:
    case RECOMP_D3D_TEXTURE_FORMAT_P8: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case RECOMP_D3D_TEXTURE_FORMAT_A8: return SDL_GPU_TEXTUREFORMAT_A8_UNORM;
    default: return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}

bool isSwizzledTextureFormat(uint32_t format_byte)
{
    return format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8 ||
           format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8;
}

struct QueuedDraw {
    SDL_GPUGraphicsPipeline *pipeline = nullptr;
    uint32_t vertex_offset = 0;
    uint32_t vertex_size = 0;
    uint32_t index_offset = 0;
    uint32_t index_size = 0;
    uint32_t index_count = 0;
    SDL_GPUTexture *texture = nullptr;
    SDL_GPUTexture *mask_texture = nullptr;
    SDL_GPUSampler *sampler0 = nullptr;
    SDL_GPUSampler *sampler1 = nullptr;
    uint32_t constant_color = 0;
    uint8_t stencil_ref = 0;
    uint32_t uniform_offset = 0;
    uint32_t vs_uniform_size = 0;
    uint32_t fs_uniform_size = 0;
    uint32_t program_offset = 0;
};

struct DrawSegment {
    SDL_GPUTexture *target_color = nullptr;
    SDL_GPUTexture *target_depth = nullptr;
    uint32_t target_color_data = 0;
    float target_w = 0.0f;
    float target_h = 0.0f;
    std::vector<uint8_t> vertex_data;
    std::vector<uint8_t> index_data;
    std::vector<float> uniform_data;
    std::vector<QueuedDraw> draws;
};

struct ShaderPair {
    SDL_GPUShader *vertex_shader = nullptr;
    SDL_GPUShader *fragment_shader = nullptr;
};

struct PipelineKey {
    uint32_t fvf = 0;
    uint32_t vertex_stride = 0;
    uint32_t program_count = 0;
    uint64_t program_hash = 0;
    bool is_strip = false;
    RecompD3dCullMode cull_mode = RECOMP_D3D_CULL_NONE;
    SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUSampleCount sample_count = SDL_GPU_SAMPLECOUNT_1;
    bool alpha_to_coverage = false;

    bool depth_test_enable = false;
    bool depth_write_enable = false;
    RecompD3dCompareFunc depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
    bool stencil_enable = false;
    uint8_t stencil_read_mask = 0xff;
    uint8_t stencil_write_mask = 0xff;
    RecompD3dCompareFunc stencil_func = RECOMP_D3D_COMPARE_ALWAYS;
    RecompD3dStencilOp stencil_fail = RECOMP_D3D_STENCIL_KEEP;
    RecompD3dStencilOp stencil_zfail = RECOMP_D3D_STENCIL_KEEP;
    RecompD3dStencilOp stencil_pass = RECOMP_D3D_STENCIL_KEEP;

    bool blend_enable = false;
    uint8_t color_write_mask = 0x0f;
    RecompD3dBlendFactor src_factor = RECOMP_D3D_BLEND_ONE;
    RecompD3dBlendFactor dst_factor = RECOMP_D3D_BLEND_ZERO;
    RecompD3dBlendOp blend_op = RECOMP_D3D_BLEND_OP_ADD;

    bool operator==(const PipelineKey &o) const {
        return fvf == o.fvf &&
               vertex_stride == o.vertex_stride &&
               program_count == o.program_count &&
               program_hash == o.program_hash &&
               is_strip == o.is_strip &&
               cull_mode == o.cull_mode &&
               color_format == o.color_format &&
               depth_format == o.depth_format &&
               sample_count == o.sample_count &&
               alpha_to_coverage == o.alpha_to_coverage &&
               depth_test_enable == o.depth_test_enable &&
               depth_write_enable == o.depth_write_enable &&
               depth_func == o.depth_func &&
               stencil_enable == o.stencil_enable &&
               stencil_read_mask == o.stencil_read_mask &&
               stencil_write_mask == o.stencil_write_mask &&
               stencil_func == o.stencil_func &&
               stencil_fail == o.stencil_fail &&
               stencil_zfail == o.stencil_zfail &&
               stencil_pass == o.stencil_pass &&
               blend_enable == o.blend_enable &&
               color_write_mask == o.color_write_mask &&
               src_factor == o.src_factor &&
               dst_factor == o.dst_factor &&
               blend_op == o.blend_op;
    }
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey &k) const {
        size_t h = k.fvf ^ (static_cast<size_t>(k.vertex_stride) << 8) ^ (static_cast<size_t>(k.program_count) << 16) ^ k.program_hash;
        h ^= (static_cast<size_t>(k.is_strip) << 24) ^ (static_cast<size_t>(k.cull_mode) << 28);
        h ^= (static_cast<size_t>(k.color_format) << 4) ^ (static_cast<size_t>(k.depth_format) << 12);
        h ^= (static_cast<size_t>(k.sample_count) << 30);
        h ^= (static_cast<size_t>(k.color_write_mask) << 20) ^ (static_cast<size_t>(k.blend_enable) << 27);
        h ^= (static_cast<size_t>(k.src_factor) << 2) ^ (static_cast<size_t>(k.dst_factor) << 6);
        h ^= (static_cast<size_t>(k.depth_test_enable) << 1) ^ (static_cast<size_t>(k.depth_func) << 8);
        h ^= (static_cast<size_t>(k.stencil_enable) << 15) ^ (static_cast<size_t>(k.stencil_func) << 18);
        return h;
    }
};

} // namespace

struct RecompD3dPresenter {
    RecompD3dPresenterConfig config{};
    std::thread::id owner_thread;
    SDL_Window *window = nullptr;
    SDL_GPUDevice *device = nullptr;
    SDL_GPUTexture *backbuffer_color = nullptr;
    SDL_GPUTexture *backbuffer_depth = nullptr;
    SDL_GPUTexture *backbuffer_resolve = nullptr;
    SDL_GPUTexture *backbuffer_copy = nullptr;
    SDL_GPUTexture *frontbuffer_copy = nullptr;
    SDL_GPUTexture *dummy_texture = nullptr;
    uint32_t msaa_count = 1u;
    SDL_GPUSampleCount msaa_samples = SDL_GPU_SAMPLECOUNT_1;

    // Persistent samplers
    SDL_GPUSampler *point_sampler = nullptr;
    SDL_GPUSampler *filter_sampler = nullptr;
    SDL_GPUSampler *program_mask_sampler = nullptr;
    SDL_GPUSampler *address_samplers[6][6]{}; // [address_u][address_v]

    // Persistent dynamic ring buffers
    SDL_GPUBuffer *draw_vertex_buffer = nullptr;
    SDL_GPUBuffer *draw_index_buffer = nullptr;
    SDL_GPUTransferBuffer *draw_vertex_transfer = nullptr;
    SDL_GPUTransferBuffer *draw_index_transfer = nullptr;
    uint32_t draw_vertex_capacity = kInitialVertexCapacity;
    uint32_t draw_index_capacity = kInitialIndexCapacity;
    uint32_t draw_vertex_used = 0u;
    uint32_t draw_index_used = 0u;

    // Current segment batch
    DrawSegment segment;

    // Active command buffer
    SDL_GPUCommandBuffer *current_command_buffer = nullptr;

    // Shader & Pipeline caches
    std::mutex shader_mutex;
    std::unordered_map<uint64_t, ShaderPair> shader_cache;
    std::unordered_map<PipelineKey, SDL_GPUGraphicsPipeline *, PipelineKeyHash> pipelines;
    std::thread precompile_thread;
    std::atomic<bool> precompile_stop{false};

    // Texture cache
    std::vector<TextureEntry> textures = std::vector<TextureEntry>(kTextureSlots);
    std::unordered_multimap<uint32_t, uint32_t> texture_index;
    uint32_t texture_count = 0u;
    uint32_t next_texture_slot = 0u;

    // Render / depth targets
    std::vector<RenderTargetEntry> render_targets;
    std::vector<DepthTargetEntry> depth_targets;
    uint64_t target_bytes = 0u;

    bool close_requested = false;
    uint32_t present_count = 0u;
    uint32_t draw_count = 0u;

    // RECOMP_PERF_COUNTER: present rate and gaps, reported once per second.
    bool performance_counter = false;
    std::chrono::steady_clock::time_point second_start{}, last_present{};
    uint32_t second_presents = 0u;
    double second_max_ms = 0.0;

    float scale = 1.0f;
    bool widescreen = true;
    bool use_spirv = false;
    bool backbuffer_resolved = false;

    // Consecutive draws usually share a pipeline; skip the map lookup.
    PipelineKey last_pkey{};
    SDL_GPUGraphicsPipeline *last_pipeline = nullptr;
};

inline uint32_t mainHeight(const RecompD3dPresenter *p) {
    return static_cast<uint32_t>(p->config.height * p->scale + 0.5f);
}

inline uint32_t presentClientWidth(const RecompD3dPresenter *p, uint32_t height) {
    return static_cast<uint32_t>(p->widescreen
        ? (height * 16u + 8u) / 9u
        : (height * 4u + 1u) / 3u);
}

inline uint32_t mainWidth(const RecompD3dPresenter *p) {
    return p->scale == 1.0f
        ? p->config.width
        : presentClientWidth(p, mainHeight(p));
}

namespace {

RecompD3dPresenter *active_presenter = nullptr;
bool immediate_present_setting = false;

// The guest paces itself to 60 Hz. On Linux (gamescope on Steam Deck),
// MAILBOX presents without a second wait for the swapchain.
bool preferMailbox(bool immediate)
{
#ifdef __linux__
    (void)immediate;
    return true;
#else
    return immediate;
#endif
}

void pumpEvents(RecompD3dPresenter *presenter)
{
    SDL_PumpEvents();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            presenter->close_requested = true;
        } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            if (!presenter->window || event.window.windowID == 0 ||
                event.window.windowID == SDL_GetWindowID(presenter->window)) {
                presenter->close_requested = true;
            }
        }
    }
}

void reportPerformance(RecompD3dPresenter *presenter)
{
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    if (presenter->second_presents++ == 0u) presenter->second_start = now;
    else presenter->second_max_ms = std::max(presenter->second_max_ms,
        std::chrono::duration<double, std::milli>(now - presenter->last_present).count());
    presenter->last_present = now;
    const double elapsed_ms = std::chrono::duration<double, std::milli>(now - presenter->second_start).count();
    if (elapsed_ms < 1000.0) return;
    const uint32_t frames = presenter->second_presents - 1u;
    std::fprintf(stderr, "recomp performance: present=%u fps=%.2f frame_ms=%.3f max_ms=%.1f draws=%u\n",
        presenter->present_count, frames * 1000.0 / elapsed_ms, elapsed_ms / frames,
        presenter->second_max_ms, presenter->draw_count / frames);
    presenter->second_presents = 1u;
    presenter->second_start = now;
    presenter->second_max_ms = 0.0;
    presenter->draw_count = 0u;
}

SDL_GPUCommandBuffer *getCommandBuffer(RecompD3dPresenter *presenter)
{
    if (!presenter->current_command_buffer) {
        presenter->current_command_buffer = SDL_AcquireGPUCommandBuffer(presenter->device);
    }
    return presenter->current_command_buffer;
}

void ensureBackbufferResolved(RecompD3dPresenter *presenter)
{
    if (!presenter->backbuffer_resolved && presenter->msaa_samples != SDL_GPU_SAMPLECOUNT_1 &&
        presenter->backbuffer_resolve && presenter->backbuffer_color &&
        presenter->backbuffer_resolve != presenter->backbuffer_color) {
        SDL_GPUCommandBuffer *cmdbuf = getCommandBuffer(presenter);
        SDL_GPUColorTargetInfo color_info{};
        color_info.texture = presenter->backbuffer_color;
        color_info.load_op = SDL_GPU_LOADOP_LOAD;
        color_info.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
        color_info.resolve_texture = presenter->backbuffer_resolve;
        SDL_GPURenderPass *rpass = SDL_BeginGPURenderPass(cmdbuf, &color_info, 1, nullptr);
        if (rpass) {
            SDL_EndGPURenderPass(rpass);
        }
        presenter->backbuffer_resolved = true;
    }
}

void unindexTexture(RecompD3dPresenter *presenter, uint32_t slot)
{
    const auto range = presenter->texture_index.equal_range(presenter->textures[slot].data);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == slot) {
            presenter->texture_index.erase(it);
            return;
        }
    }
}

RenderTargetEntry *findRenderTarget(RecompD3dPresenter *presenter, const RecompD3dTextureDesc &desc)
{
    for (auto &entry : presenter->render_targets) {
        if (entry.desc.data == desc.data && entry.desc.width == desc.width && entry.desc.height == desc.height) {
            return &entry;
        }
    }
    return nullptr;
}

SDL_GPUTexture *lookupDepthTarget(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterTarget &target,
    uint32_t color_width,
    uint32_t color_height)
{
    if (target.no_depth) {
        return nullptr;
    }
    if (!target.custom_depth) {
        if (target.offscreen) return nullptr;
        return (color_width == mainWidth(presenter) && color_height == mainHeight(presenter))
            ? presenter->backbuffer_depth : nullptr;
    }

    const uint32_t width = target.depth.width;
    const uint32_t height = target.depth.height;

    if (width != color_width || height != color_height) {
        return nullptr;
    }

    const RecompD3dTextureDesc &desc = target.depth;
    if (desc.data == 0u || width == 0u || height == 0u) {
        return nullptr;
    }

    for (const auto &entry : presenter->depth_targets) {
        if (entry.desc.data == desc.data && entry.width == width && entry.height == height) {
            return entry.texture;
        }
    }

    const uint64_t bytes = static_cast<uint64_t>(width) * height * 4u;
    if (bytes > kTargetByteLimit - presenter->target_bytes) {
        return nullptr;
    }

    SDL_GPUTextureCreateInfo dt_info{};
    dt_info.type = SDL_GPU_TEXTURETYPE_2D;
    dt_info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    dt_info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    dt_info.width = width;
    dt_info.height = height;
    dt_info.layer_count_or_depth = 1;
    dt_info.num_levels = 1;
    SDL_GPUTexture *new_dt = SDL_CreateGPUTexture(presenter->device, &dt_info);
    if (!new_dt) return nullptr;

    presenter->depth_targets.push_back({desc, new_dt, width, height});
    presenter->target_bytes += bytes;
    return new_dt;
}

SDL_GPUSampler *lookupDrawSampler(RecompD3dPresenter *presenter, uint32_t address_u, uint32_t address_v)
{
    uint32_t u = address_u <= 5u ? address_u : 1u;
    uint32_t v = address_v <= 5u ? address_v : 1u;
    if (presenter->address_samplers[u][v] != nullptr) {
        return presenter->address_samplers[u][v];
    }

    SDL_GPUSamplerCreateInfo desc{};
    desc.min_filter = SDL_GPU_FILTER_LINEAR;
    desc.mag_filter = SDL_GPU_FILTER_LINEAR;
    desc.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    desc.address_mode_u = hostAddressMode(u);
    desc.address_mode_v = hostAddressMode(v);
    desc.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    desc.enable_anisotropy = false;
    presenter->address_samplers[u][v] = SDL_CreateGPUSampler(presenter->device, &desc);
    return presenter->address_samplers[u][v] ? presenter->address_samplers[u][v] : presenter->filter_sampler;
}

bool ensureDynamicBuffers(RecompD3dPresenter *presenter, uint32_t req_vbytes, uint32_t req_ibytes)
{
    if (req_vbytes > presenter->draw_vertex_capacity) {
        if (presenter->current_command_buffer) {
            SDL_SubmitGPUCommandBuffer(presenter->current_command_buffer);
            presenter->current_command_buffer = nullptr;
        }
        uint32_t new_cap = std::max(req_vbytes * 2u, presenter->draw_vertex_capacity * 2u);
        if (presenter->draw_vertex_buffer) SDL_ReleaseGPUBuffer(presenter->device, presenter->draw_vertex_buffer);
        if (presenter->draw_vertex_transfer) SDL_ReleaseGPUTransferBuffer(presenter->device, presenter->draw_vertex_transfer);

        SDL_GPUBufferCreateInfo binfo{SDL_GPU_BUFFERUSAGE_VERTEX, new_cap, 0};
        presenter->draw_vertex_buffer = SDL_CreateGPUBuffer(presenter->device, &binfo);
        SDL_GPUTransferBufferCreateInfo tinfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, new_cap, 0};
        presenter->draw_vertex_transfer = SDL_CreateGPUTransferBuffer(presenter->device, &tinfo);
        presenter->draw_vertex_capacity = new_cap;
        presenter->draw_vertex_used = 0u;
        if (!presenter->draw_vertex_buffer || !presenter->draw_vertex_transfer) return false;
    }

    if (req_ibytes > presenter->draw_index_capacity) {
        if (presenter->current_command_buffer) {
            SDL_SubmitGPUCommandBuffer(presenter->current_command_buffer);
            presenter->current_command_buffer = nullptr;
        }
        uint32_t new_cap = std::max(req_ibytes * 2u, presenter->draw_index_capacity * 2u);
        if (presenter->draw_index_buffer) SDL_ReleaseGPUBuffer(presenter->device, presenter->draw_index_buffer);
        if (presenter->draw_index_transfer) SDL_ReleaseGPUTransferBuffer(presenter->device, presenter->draw_index_transfer);

        SDL_GPUBufferCreateInfo binfo{SDL_GPU_BUFFERUSAGE_INDEX, new_cap, 0};
        presenter->draw_index_buffer = SDL_CreateGPUBuffer(presenter->device, &binfo);
        SDL_GPUTransferBufferCreateInfo tinfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, new_cap, 0};
        presenter->draw_index_transfer = SDL_CreateGPUTransferBuffer(presenter->device, &tinfo);
        presenter->draw_index_capacity = new_cap;
        presenter->draw_index_used = 0u;
        if (!presenter->draw_index_buffer || !presenter->draw_index_transfer) return false;
    }
    return true;
}

SDL_GPUTexture *prepareTexture(RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand &draw)
{
    const RecompD3dTextureDesc &desc = draw.texture;
    if (!draw.has_texture || desc.data == 0u || desc.width == 0u || desc.height == 0u) {
        return nullptr;
    }
    if (draw.texture_is_backbuffer) {
        if (!presenter->backbuffer_copy) return presenter->dummy_texture;
        ensureBackbufferResolved(presenter);
        SDL_GPUCommandBuffer *cmdbuf = getCommandBuffer(presenter);
        SDL_GPUCopyPass *copypass = SDL_BeginGPUCopyPass(cmdbuf);
        if (copypass) {
            SDL_GPUTextureLocation src_loc{presenter->backbuffer_resolve, 0, 0, 0, 0};
            SDL_GPUTextureLocation dst_loc{presenter->backbuffer_copy, 0, 0, 0, 0};
            SDL_CopyGPUTextureToTexture(copypass, &src_loc, &dst_loc, mainWidth(presenter), mainHeight(presenter), 1, false);
            SDL_EndGPUCopyPass(copypass);
        }
        return presenter->backbuffer_copy;
    }
    if (draw.texture_is_frontbuffer) {
        if (!presenter->frontbuffer_copy) return presenter->dummy_texture;
        return presenter->frontbuffer_copy;
    }

    RenderTargetEntry *rt = findRenderTarget(presenter, desc);
    if (rt && rt->texture) {
        return rt->texture;
    }

    const bool palettized = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_P8;
    const bool linear_bgra = desc.format_byte == 0x12u;
    const bool compressed = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT1 ||
                            desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT3 ||
                            desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT5;
    const uint32_t levels = compressed && desc.mip_levels ? desc.mip_levels : 1u;

    if (palettized && (draw.palette_bytes == nullptr || draw.palette_byte_count != kPaletteBytes)) {
        return nullptr;
    }

    const auto cached = presenter->texture_index.equal_range(desc.data);
    const bool fingerprinted = !linear_bgra && draw.texture_bytes != nullptr;
    const uint64_t fingerprint = fingerprinted
        ? textureFingerprint(draw.texture_bytes, draw.texture_byte_count) : 0u;

    for (auto it = cached.first; it != cached.second; ++it) {
        TextureEntry &entry = presenter->textures[it->second];
        if (entry.format_byte == desc.format_byte &&
            entry.width == desc.width && entry.height == desc.height && entry.mip_levels == levels &&
            (!palettized || std::memcmp(entry.palette, draw.palette_bytes, kPaletteBytes) == 0)) {
            if (fingerprinted && entry.fingerprint != fingerprint) {
                unindexTexture(presenter, it->second);
                if (entry.texture) SDL_ReleaseGPUTexture(presenter->device, entry.texture);
                entry.used = false;
                break;
            }
            if (linear_bgra && draw.texture_bytes != nullptr && draw.texture_byte_count > 0u) {
                const uint32_t tbuf_size = desc.width * desc.height * 4u;
                SDL_GPUTransferBufferCreateInfo tbuf_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, tbuf_size, 0};
                SDL_GPUTransferBuffer *tbuf = SDL_CreateGPUTransferBuffer(presenter->device, &tbuf_info);
                if (tbuf) {
                    void *map = SDL_MapGPUTransferBuffer(presenter->device, tbuf, false);
                    if (map) {
                        const auto *src = static_cast<const uint8_t *>(draw.texture_bytes);
                        auto *dst = static_cast<uint8_t *>(map);
                        for (uint32_t y = 0u; y < desc.height; ++y) {
                            std::memcpy(dst + y * (desc.width * 4u), src + y * desc.pitch, desc.width * 4u);
                        }
                        SDL_UnmapGPUTransferBuffer(presenter->device, tbuf);
                        SDL_GPUCommandBuffer *cmdbuf = getCommandBuffer(presenter);
                        SDL_GPUCopyPass *cpass = SDL_BeginGPUCopyPass(cmdbuf);
                        if (cpass) {
                            SDL_GPUTextureTransferInfo src_info{tbuf, 0, desc.width, desc.height};
                            SDL_GPUTextureRegion dst_reg{entry.texture, 0, 0, 0, 0, 0, desc.width, desc.height, 1};
                            SDL_UploadToGPUTexture(cpass, &src_info, &dst_reg, false);
                            SDL_EndGPUCopyPass(cpass);
                        }
                    }
                    SDL_ReleaseGPUTransferBuffer(presenter->device, tbuf);
                }
            }
            return entry.texture;
        }
    }

    const SDL_GPUTextureFormat format = linear_bgra
        ? SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM : hostTextureFormat(desc.format_byte);
    if (format == SDL_GPU_TEXTUREFORMAT_INVALID || draw.texture_bytes == nullptr ||
        draw.texture_byte_count == 0u) {
        return nullptr;
    }

    std::vector<uint8_t> unswizzled;
    const void *upload_bytes = draw.texture_bytes;
    uint32_t upload_size = draw.texture_byte_count;

    if (linear_bgra) {
        const size_t packed_size = static_cast<size_t>(desc.width) * desc.height * 4u;
        unswizzled.resize(packed_size);
        const auto *src = static_cast<const uint8_t *>(draw.texture_bytes);
        for (uint32_t y = 0u; y < desc.height; ++y) {
            std::memcpy(unswizzled.data() + y * (desc.width * 4u), src + y * desc.pitch, desc.width * 4u);
        }
        upload_bytes = unswizzled.data();
        upload_size = static_cast<uint32_t>(packed_size);
    } else if (palettized) {
        const size_t texels = static_cast<size_t>(desc.width) * desc.height;
        if (texels > draw.texture_byte_count) return nullptr;
        std::vector<uint8_t> indices(texels);
        if (!recomp_d3d_texture_unswizzle(
                static_cast<const uint8_t *>(draw.texture_bytes),
                indices.data(), desc.width, desc.height, 1u)) {
            return nullptr;
        }
        unswizzled.resize(texels * 4u);
        const auto *palette = static_cast<const uint8_t *>(draw.palette_bytes);
        for (size_t i = 0u; i < texels; ++i) {
            std::memcpy(unswizzled.data() + i * 4u, palette + indices[i] * 4u, 4u);
        }
        upload_bytes = unswizzled.data();
        upload_size = static_cast<uint32_t>(unswizzled.size());
    } else if (isSwizzledTextureFormat(desc.format_byte) && !desc.linear) {
        const size_t texel_bytes = desc.bits_per_pixel / 8u;
        unswizzled.resize(static_cast<size_t>(desc.width) * desc.height * texel_bytes);
        if (!recomp_d3d_texture_unswizzle(
                static_cast<const uint8_t *>(draw.texture_bytes),
                unswizzled.data(), desc.width, desc.height, static_cast<uint32_t>(texel_bytes))) {
            return nullptr;
        }
        upload_bytes = unswizzled.data();
        upload_size = static_cast<uint32_t>(unswizzled.size());
    }

    SDL_GPUTextureCreateInfo tex_info{};
    tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format = format;
    tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex_info.width = desc.width;
    tex_info.height = desc.height;
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels = levels;
    SDL_GPUTexture *gpu_tex = SDL_CreateGPUTexture(presenter->device, &tex_info);
    if (!gpu_tex) return nullptr;

    SDL_GPUTransferBufferCreateInfo tbuf_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, upload_size, 0};
    SDL_GPUTransferBuffer *tbuf = SDL_CreateGPUTransferBuffer(presenter->device, &tbuf_info);
    if (!tbuf) {
        SDL_ReleaseGPUTexture(presenter->device, gpu_tex);
        return nullptr;
    }

    void *map = SDL_MapGPUTransferBuffer(presenter->device, tbuf, false);
    if (map) {
        std::memcpy(map, upload_bytes, upload_size);
        SDL_UnmapGPUTransferBuffer(presenter->device, tbuf);

        SDL_GPUCommandBuffer *cmdbuf = getCommandBuffer(presenter);
        SDL_GPUCopyPass *cpass = SDL_BeginGPUCopyPass(cmdbuf);
        if (cpass) {
            if (compressed) {
                const uint32_t block_bytes = (desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT1) ? 8u : 16u;
                uint32_t cur_w = desc.width, cur_h = desc.height, cur_offset = 0u;
                for (uint32_t lvl = 0; lvl < levels; ++lvl) {
                    SDL_GPUTextureTransferInfo src_info{tbuf, cur_offset, 0, 0};
                    SDL_GPUTextureRegion dst_reg{gpu_tex, lvl, 0, 0, 0, 0, cur_w, cur_h, 1};
                    SDL_UploadToGPUTexture(cpass, &src_info, &dst_reg, false);

                    uint32_t row_pitch = ((cur_w + 3u) / 4u) * block_bytes;
                    cur_offset += row_pitch * ((cur_h + 3u) / 4u);
                    cur_w = cur_w > 1u ? cur_w / 2u : 1u;
                    cur_h = cur_h > 1u ? cur_h / 2u : 1u;
                }
            } else {
                SDL_GPUTextureTransferInfo src_info{tbuf, 0, desc.width, desc.height};
                SDL_GPUTextureRegion dst_reg{gpu_tex, 0, 0, 0, 0, 0, desc.width, desc.height, 1};
                SDL_UploadToGPUTexture(cpass, &src_info, &dst_reg, false);
            }
            SDL_EndGPUCopyPass(cpass);
        }
    }
    SDL_ReleaseGPUTransferBuffer(presenter->device, tbuf);

    const uint32_t slot = presenter->next_texture_slot;
    TextureEntry &entry = presenter->textures[slot];
    if (entry.used) {
        unindexTexture(presenter, slot);
        if (entry.texture) SDL_ReleaseGPUTexture(presenter->device, entry.texture);
    }
    entry.used = true;
    entry.data = desc.data;
    entry.format_byte = desc.format_byte;
    entry.width = desc.width;
    entry.height = desc.height;
    entry.mip_levels = levels;
    entry.fingerprint = fingerprint;
    entry.texture = gpu_tex;
    if (palettized) std::memcpy(entry.palette, draw.palette_bytes, kPaletteBytes);

    presenter->texture_index.insert({desc.data, slot});
    presenter->next_texture_slot = (presenter->next_texture_slot + 1u) % kTextureSlots;
    if (presenter->texture_count < kTextureSlots) ++presenter->texture_count;
    return gpu_tex;
}

std::string buildMslShader(const RecompD3dVertexLayout &layout, uint32_t fvf,
                           uint32_t program_count, const uint32_t (*program_tokens)[4])
{
    const bool has_normal = layout.normal_offset != RECOMP_D3D_FVF_ABSENT;
    const bool has_diffuse = layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT;
    const bool has_texcoord = layout.texcoord_offset != RECOMP_D3D_FVF_ABSENT;
    const bool four_coords = (layout.texcoord_count == 4u);
    const bool two_coords = (layout.texcoord_count == 2u);

    std::string s;
    s.reserve(4096);
    s += kMslShaderPrologue;

    // VSIn
    s += "struct VSIn {\n";
    s += layout.pretransformed ? "    float4 position [[attribute(0)]];\n" : "    float3 position [[attribute(0)]];\n";
    if (layout.blend_weight_count == 1u) {
        s += "    float weights [[attribute(1)]];\n";
    } else if (layout.blend_weight_count == 2u) {
        s += "    float2 weights [[attribute(1)]];\n";
    } else if (layout.blend_weight_count == 3u) {
        s += "    float3 weights [[attribute(1)]];\n";
    }
    if (has_normal) s += "    float3 normal [[attribute(2)]];\n";
    if (has_diffuse) s += "    float4 diffuse [[attribute(3)]];\n";
    if (has_texcoord) s += "    float2 texcoord [[attribute(4)]];\n";
    if (four_coords) {
        s += "    float2 texcoord1 [[attribute(5)]];\n";
        s += "    float2 texcoord2 [[attribute(6)]];\n";
        s += "    float2 texcoord3 [[attribute(7)]];\n";
    } else if (two_coords) {
        s += "    float2 texcoord1 [[attribute(5)]];\n";
    }
    s += "};\n";

    // VSOut
    s += "struct VSOut {\n";
    s += "    float4 position [[position]];\n";
    s += "    float4 color [[user(col0)]];\n";
    s += "    float2 texcoord [[user(uv0)]];\n";
    s += "    float2 reflection_coord [[user(refl_uv)]];\n";
    if (four_coords) {
        s += "    float2 texcoord1 [[user(uv1)]];\n";
        s += "    float2 texcoord2 [[user(uv2)]];\n";
        s += "    float2 texcoord3 [[user(uv3)]];\n";
    } else if (two_coords) {
        s += "    float2 texcoord1 [[user(uv1)]];\n";
    }
    if (program_count) {
        s += "    float2 program_q [[user(prog_q)]];\n";
    }
    s += "};\n";

    // vs_main
    s += program_count
        ? "vertex VSOut vs_main(VSIn in [[stage_in]], constant TransformUniforms &u [[buffer(0)]],\n"
          "                    constant ProgramConstants &pc [[buffer(1)]]) {\n"
        : "vertex VSOut vs_main(VSIn in [[stage_in]], constant TransformUniforms &u [[buffer(0)]]) {\n";
    s += "    VSOut out;\n";

    bool has_prog = false;
    if (program_count) {
        std::string prog_body;
        if (recomp_d3d_vertex_program_msl_source(program_tokens, program_count, prog_body)) {
            s += prog_body;
            s += "    return out;\n";
            s += "}\n";
            has_prog = true;
        }
    }
    if (!has_prog) {
        if (layout.pretransformed) {
            s += "    out.position = (u.wvp[0] * float4(in.position.xyz, 1.0)) / in.position.w;\n";
        } else if (layout.blend_weight_count != 0u) {
            s += "    if (u.blend_flags.x > 0.5) {\n";
            s += "        float4 p = float4(0.0);\n";
            s += "        float remainder = 1.0;\n";
            if (layout.blend_weight_count == 1u) {
                s += "        p += in.weights * (u.wvp[0] * float4(in.position, 1.0));\n";
                s += "        remainder -= in.weights;\n";
            } else {
                for (uint32_t i = 0u; i < layout.blend_weight_count; ++i) {
                    s += "        p += in.weights[" + std::to_string(i) + "] * (u.wvp[" + std::to_string(i) + "] * float4(in.position, 1.0));\n";
                    s += "        remainder -= in.weights[" + std::to_string(i) + "];\n";
                }
            }
            s += "        p += remainder * (u.wvp[" + std::to_string(layout.blend_weight_count) + "] * float4(in.position, 1.0));\n";
            s += "        out.position = p;\n";
            s += "    } else {\n";
            s += "        out.position = u.wvp[0] * float4(in.position, 1.0);\n";
            s += "    }\n";
        } else {
            s += "    out.position = u.wvp[0] * float4(in.position, 1.0);\n";
        }

        // Color & lighting
        if (has_diffuse) {
            s += "    out.color = in.diffuse.zyxw;\n";
        } else if (has_normal && !layout.pretransformed) {
            s += "    if (u.directional_flags.x > 0.5) {\n";
            s += "        float3 n = (u.directional_normals[0] * float4(in.normal, 0.0)).xyz;\n";
            if (layout.blend_weight_count != 0u) {
                s += "        if (u.blend_flags.x > 0.5) {\n";
                s += "            n = float3(0.0);\n";
                s += "            float remainder = 1.0;\n";
                if (layout.blend_weight_count == 1u) {
                    s += "            n += in.weights * (u.directional_normals[0] * float4(in.normal, 0.0)).xyz;\n";
                    s += "            remainder -= in.weights;\n";
                } else {
                    for (uint32_t i = 0u; i < layout.blend_weight_count; ++i) {
                        s += "            n += in.weights[" + std::to_string(i) + "] * (u.directional_normals[" + std::to_string(i) + "] * float4(in.normal, 0.0)).xyz;\n";
                        s += "            remainder -= in.weights[" + std::to_string(i) + "];\n";
                    }
                }
                s += "            n += remainder * (u.directional_normals[" + std::to_string(layout.blend_weight_count) + "] * float4(in.normal, 0.0)).xyz;\n";
                s += "        }\n";
            }
            s += "        if (u.directional_flags.y > 0.5 && dot(n, n) > 0.0) n = normalize(n);\n";
            s += "        float3 rgb = u.directional_base.rgb;\n";
            s += "        uint light_cnt = uint(u.directional_flags.z);\n";
            s += "        for (uint i = 0; i < light_cnt; ++i) {\n";
            s += "            rgb += u.directional_material.rgb * u.directional_colors[i].rgb * max(0.0, dot(n, u.directional_directions[i].xyz));\n";
            s += "        }\n";
            s += "        out.color = float4(saturate(rgb), 1.0);\n";
            s += "    } else {\n";
            s += "        out.color = float4(abs(normalize(in.normal)), 1.0);\n";
            s += "    }\n";
        } else {
            s += "    out.color = float4(0.75, 0.75, 0.78, 1.0);\n";
        }

        s += has_texcoord ? "    out.texcoord = in.texcoord;\n" : "    out.texcoord = float2(0.0);\n";
        if (four_coords) {
            s += "    out.texcoord1 = in.texcoord1;\n";
            s += "    out.texcoord2 = in.texcoord2;\n";
            s += "    out.texcoord3 = in.texcoord3;\n";
        } else if (two_coords) {
            s += "    out.texcoord1 = in.texcoord1;\n";
        }

        s += "    out.reflection_coord = float2(0.0);\n";
        if (has_texcoord && !layout.pretransformed) {
            if (has_normal) {
                s += "    if (u.reflection_flags.x > 1.5) {\n";
                s += "        out.reflection_coord = in.texcoord;\n";
                s += "    } else if (u.reflection_flags.x > 0.5) {\n";
                s += "        float3 eye = (u.reflection_world_view * float4(in.position, 1.0)).xyz;\n";
                s += "        float3 n = (u.reflection_normal * float4(in.normal, 0.0)).xyz;\n";
                s += "        if (u.reflection_flags.y > 0.5) n = normalize(n);\n";
                s += "        float3 r = reflect(normalize(eye), n);\n";
                s += "        out.reflection_coord = (u.reflection_transform * float4(r, 1.0)).xy;\n";
                s += "    }\n";
            } else {
                s += "    if (u.reflection_flags.x > 1.5) {\n";
                s += "        out.reflection_coord = in.texcoord;\n";
                s += "    } else if (u.reflection_flags.x > 0.5) {\n";
                s += "        float3 eye = (u.reflection_world_view * float4(in.position, 1.0)).xyz;\n";
                s += "        float3 r = normalize(eye);\n";
                s += "        out.reflection_coord = (u.reflection_transform * float4(r, 1.0)).xy;\n";
                s += "    }\n";
            }
        }
        s += "    return out;\n";
        s += "}\n";
    }

    // ps_main
    s += "fragment float4 ps_main(VSOut in [[stage_in]],\n";
    s += "                        constant TransformUniforms &u [[buffer(0)]],\n";
    s += "                        texture2d<float> guest_texture [[texture(0)]],\n";
    s += "                        sampler guest_sampler [[sampler(0)]],\n";
    s += "                        texture2d<float> alpha_mask [[texture(1)]],\n";
    s += "                        sampler mask_sampler [[sampler(1)]]) {\n";
    if (program_count) {
        s += "    in.texcoord /= in.program_q.x;\n";
        s += "    if (u.reflection_flags.z > 0.5) in.reflection_coord /= in.program_q.y;\n";
    }
    s += "    float4 shaded = in.color;\n";
    s += "    if (u.reflection_flags.x > 0.5) {\n";
    s += "        float4 base = guest_texture.sample(guest_sampler, in.texcoord);\n";
    s += "        base.a *= u.reflection_diffuse.a;\n";
    s += "        float4 env = alpha_mask.sample(guest_sampler, in.reflection_coord);\n";
    s += "        float4 diffuse = u.reflection_diffuse;\n";
    s += "        if (u.directional_flags.x > 0.5) diffuse.rgb = in.color.rgb;\n";
    s += "        shaded = mix(base, env, env.a) * diffuse;\n";
    s += "    } else\n";
    if (four_coords) {
        s += "    if (u.texture_flags.z > 0.5) {\n";
        s += "        float4 t0 = guest_texture.sample(guest_sampler, in.texcoord * u.texture_flags.xy);\n";
        s += "        float4 t1 = guest_texture.sample(guest_sampler, in.texcoord1 * u.texture_flags.xy);\n";
        s += "        float4 t2 = guest_texture.sample(guest_sampler, in.texcoord2 * u.texture_flags.xy);\n";
        s += "        float4 t3 = guest_texture.sample(guest_sampler, in.texcoord3 * u.texture_flags.xy);\n";
        s += "        shaded = 0.5 * (saturate((128.0 / 255.0) * (t0 + t1)) + saturate((128.0 / 255.0) * (t2 + t3)));\n";
        s += "    } else\n";
    } else if (two_coords) {
        s += "    if (u.texture_flags.w > 0.5) {\n";
        s += "        float3 rgb = guest_texture.sample(guest_sampler, in.texcoord1 * u.texture_flags.xy).rgb;\n";
        s += "        float alpha = alpha_mask.sample(mask_sampler, in.texcoord * u.lighting_flags.zw).a;\n";
        s += "        shaded = in.color * float4(rgb, alpha);\n";
        s += "    } else\n";
    }
    s += "    if (u.blend_flags.y == 1.0) {\n";
    s += "        shaded = u.texture_factor;\n";
    s += "    } else if (u.draw_flags.x > 0.5) {\n";
    s += "        float4 sampled = guest_texture.sample(guest_sampler, in.texcoord * u.texture_flags.xy);\n";
    s += "        if (u.lighting_flags.y > 0.5) sampled.rgb = float3(1.0);\n";
    s += has_diffuse ? "        shaded *= sampled;\n" : "        shaded = sampled;\n";
    s += "        if (u.directional_flags.x > 0.5) shaded.rgb *= in.color.rgb;\n";
    s += "        if (u.lighting_flags.x > 0.5) shaded.rgb = float3(0.0);\n";
    s += "        shaded.a = (u.blend_flags.w > 0.5) ? u.blend_flags.z : (shaded.a * u.blend_flags.z);\n";
    s += "        if (u.blend_flags.y > 1.5) shaded *= u.texture_factor;\n";
    s += "    }\n";
    s += "    if (u.reflection_flags.z > 0.5) {\n";
    s += "        shaded.a *= alpha_mask.sample(mask_sampler, in.reflection_coord, bias(u.reflection_flags.w)).a;\n";
    s += "    }\n";
    s += "    if (u.draw_flags.y > 0.5) {\n";
    s += "        float alpha = round(saturate(shaded.a) * 255.0);\n";
    s += "        float ref = u.draw_flags.w;\n";
    s += "        int func = int(u.draw_flags.z);\n";
    s += "        bool alpha_pass = (func == 7) ||\n";
    s += "            (func == 1 && alpha < ref) ||\n";
    s += "            (func == 2 && alpha == ref) ||\n";
    s += "            (func == 3 && alpha <= ref) ||\n";
    s += "            (func == 4 && alpha > ref) ||\n";
    s += "            (func == 5 && alpha != ref) ||\n";
    s += "            (func == 6 && alpha >= ref);\n";
    s += "        if (!alpha_pass) discard_fragment();\n";
    s += "    }\n";
    s += "    return shaded;\n";
    s += "}\n";

    return s;
}

static const char kGlslUniformsVertex[] = R"(
layout(set = 1, binding = 0, std140) uniform TransformUniforms {
    mat4 wvp[4];
    vec4 draw_flags;
    vec4 blend_flags;
    vec4 texture_factor;
    vec4 lighting_flags;
    vec4 texture_flags;
    mat4 reflection_world_view;
    mat4 reflection_normal;
    mat4 reflection_transform;
    vec4 reflection_diffuse;
    vec4 reflection_flags;
    vec4 directional_flags;
    mat4 directional_normals[4];
    vec4 directional_base;
    vec4 directional_material;
    vec4 directional_directions[8];
    vec4 directional_colors[8];
} u;
)";

static const char kGlslUniformsFragment[] = R"(
layout(set = 3, binding = 0, std140) uniform TransformUniforms {
    mat4 wvp[4];
    vec4 draw_flags;
    vec4 blend_flags;
    vec4 texture_factor;
    vec4 lighting_flags;
    vec4 texture_flags;
    mat4 reflection_world_view;
    mat4 reflection_normal;
    mat4 reflection_transform;
    vec4 reflection_diffuse;
    vec4 reflection_flags;
    vec4 directional_flags;
    mat4 directional_normals[4];
    vec4 directional_base;
    vec4 directional_material;
    vec4 directional_directions[8];
    vec4 directional_colors[8];
} u;
)";

std::string buildGlslVertexShader(const RecompD3dVertexLayout &layout, uint32_t fvf,
                                  uint32_t program_count, const uint32_t (*program_tokens)[4])
{
    const bool has_normal = layout.normal_offset != RECOMP_D3D_FVF_ABSENT;
    const bool has_diffuse = layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT;
    const bool has_texcoord = layout.texcoord_offset != RECOMP_D3D_FVF_ABSENT;
    const bool four_coords = (layout.texcoord_count == 4u);
    const bool two_coords = (layout.texcoord_count == 2u);

    std::string s;
    s.reserve(4096);
    s += "#version 450\n";
    s += kGlslUniformsVertex;
    if (program_count) {
        s += "layout(set = 1, binding = 1, std140) uniform ProgramConstants { vec4 vc[192]; } pc;\n";
    }

    if (layout.pretransformed) {
        s += "layout(location = 0) in vec4 in_position;\n";
    } else {
        s += "layout(location = 0) in vec3 in_position;\n";
    }

    if (layout.blend_weight_count == 1u) {
        s += "layout(location = 1) in float in_weights;\n";
    } else if (layout.blend_weight_count == 2u) {
        s += "layout(location = 1) in vec2 in_weights;\n";
    } else if (layout.blend_weight_count == 3u) {
        s += "layout(location = 1) in vec3 in_weights;\n";
    }

    if (has_normal) s += "layout(location = 2) in vec3 in_normal;\n";
    if (has_diffuse) s += "layout(location = 3) in vec4 in_diffuse;\n";
    if (has_texcoord) s += "layout(location = 4) in vec2 in_texcoord;\n";
    if (four_coords) {
        s += "layout(location = 5) in vec2 in_texcoord1;\n";
        s += "layout(location = 6) in vec2 in_texcoord2;\n";
        s += "layout(location = 7) in vec2 in_texcoord3;\n";
    } else if (two_coords) {
        s += "layout(location = 5) in vec2 in_texcoord1;\n";
    }

    s += "layout(location = 0) out vec4 v_color;\n";
    s += "layout(location = 1) out vec2 v_texcoord;\n";
    s += "layout(location = 2) out vec2 v_reflection_coord;\n";
    if (four_coords) {
        s += "layout(location = 3) out vec2 v_texcoord1;\n";
        s += "layout(location = 4) out vec2 v_texcoord2;\n";
        s += "layout(location = 5) out vec2 v_texcoord3;\n";
    } else if (two_coords) {
        s += "layout(location = 3) out vec2 v_texcoord1;\n";
    }
    if (program_count) {
        s += "layout(location = 6) out vec2 v_program_q;\n";
    }

    s += "void main() {\n";

    bool has_prog = false;
    if (program_count) {
        std::string prog_body;
        if (recomp_d3d_vertex_program_glsl_source(program_tokens, program_count, prog_body)) {
            s += prog_body;
            s += "}\n";
            has_prog = true;
        }
    }

    if (!has_prog) {
        if (layout.pretransformed) {
            s += "    gl_Position = (u.wvp[0] * vec4(in_position.xyz, 1.0)) / in_position.w;\n";
        } else if (layout.blend_weight_count != 0u) {
            s += "    if (u.blend_flags.x > 0.5) {\n";
            s += "        vec4 p = vec4(0.0);\n";
            s += "        float remainder = 1.0;\n";
            if (layout.blend_weight_count == 1u) {
                s += "        p += in_weights * (u.wvp[0] * vec4(in_position, 1.0));\n";
                s += "        remainder -= in_weights;\n";
            } else {
                for (uint32_t i = 0u; i < layout.blend_weight_count; ++i) {
                    s += "        p += in_weights[" + std::to_string(i) + "] * (u.wvp[" + std::to_string(i) + "] * vec4(in_position, 1.0));\n";
                    s += "        remainder -= in_weights[" + std::to_string(i) + "];\n";
                }
            }
            s += "        p += remainder * (u.wvp[" + std::to_string(layout.blend_weight_count) + "] * vec4(in_position, 1.0));\n";
            s += "        gl_Position = p;\n";
            s += "    } else {\n";
            s += "        gl_Position = u.wvp[0] * vec4(in_position, 1.0);\n";
            s += "    }\n";
        } else {
            s += "    gl_Position = u.wvp[0] * vec4(in_position, 1.0);\n";
        }

        // Color & lighting
        if (has_diffuse) {
            s += "    v_color = in_diffuse.zyxw;\n";
        } else if (has_normal && !layout.pretransformed) {
            s += "    if (u.directional_flags.x > 0.5) {\n";
            s += "        vec3 n = (u.directional_normals[0] * vec4(in_normal, 0.0)).xyz;\n";
            if (layout.blend_weight_count != 0u) {
                s += "        if (u.blend_flags.x > 0.5) {\n";
                s += "            n = vec3(0.0);\n";
                s += "            float remainder = 1.0;\n";
                if (layout.blend_weight_count == 1u) {
                    s += "            n += in_weights * (u.directional_normals[0] * vec4(in_normal, 0.0)).xyz;\n";
                    s += "            remainder -= in_weights;\n";
                } else {
                    for (uint32_t i = 0u; i < layout.blend_weight_count; ++i) {
                        s += "            n += in_weights[" + std::to_string(i) + "] * (u.directional_normals[" + std::to_string(i) + "] * vec4(in_normal, 0.0)).xyz;\n";
                        s += "            remainder -= in_weights[" + std::to_string(i) + "];\n";
                    }
                }
                s += "            n += remainder * (u.directional_normals[" + std::to_string(layout.blend_weight_count) + "] * vec4(in_normal, 0.0)).xyz;\n";
                s += "        }\n";
            }
            s += "        if (u.directional_flags.y > 0.5 && dot(n, n) > 0.0) n = normalize(n);\n";
            s += "        vec3 rgb = u.directional_base.rgb;\n";
            s += "        uint light_cnt = uint(u.directional_flags.z);\n";
            s += "        for (uint i = 0u; i < light_cnt; ++i) {\n";
            s += "            rgb += u.directional_material.rgb * u.directional_colors[i].rgb * max(0.0, dot(n, u.directional_directions[i].xyz));\n";
            s += "        }\n";
            s += "        v_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n";
            s += "    } else {\n";
            s += "        v_color = vec4(abs(normalize(in_normal)), 1.0);\n";
            s += "    }\n";
        } else {
            s += "    v_color = vec4(0.75, 0.75, 0.78, 1.0);\n";
        }

        s += has_texcoord ? "    v_texcoord = in_texcoord;\n" : "    v_texcoord = vec2(0.0);\n";
        if (four_coords) {
            s += "    v_texcoord1 = in_texcoord1;\n";
            s += "    v_texcoord2 = in_texcoord2;\n";
            s += "    v_texcoord3 = in_texcoord3;\n";
        } else if (two_coords) {
            s += "    v_texcoord1 = in_texcoord1;\n";
        }

        s += "    v_reflection_coord = vec2(0.0);\n";
        if (has_texcoord && !layout.pretransformed) {
            if (has_normal) {
                s += "    if (u.reflection_flags.x > 1.5) {\n";
                s += "        v_reflection_coord = in_texcoord;\n";
                s += "    } else if (u.reflection_flags.x > 0.5) {\n";
                s += "        vec3 eye = (u.reflection_world_view * vec4(in_position, 1.0)).xyz;\n";
                s += "        vec3 n = (u.reflection_normal * vec4(in_normal, 0.0)).xyz;\n";
                s += "        if (u.reflection_flags.y > 0.5) n = normalize(n);\n";
                s += "        vec3 r = reflect(normalize(eye), n);\n";
                s += "        v_reflection_coord = (u.reflection_transform * vec4(r, 1.0)).xy;\n";
                s += "    }\n";
            } else {
                s += "    if (u.reflection_flags.x > 1.5) {\n";
                s += "        v_reflection_coord = in_texcoord;\n";
                s += "    } else if (u.reflection_flags.x > 0.5) {\n";
                s += "        vec3 eye = (u.reflection_world_view * vec4(in_position, 1.0)).xyz;\n";
                s += "        vec3 r = normalize(eye);\n";
                s += "        v_reflection_coord = (u.reflection_transform * vec4(r, 1.0)).xy;\n";
                s += "    }\n";
            }
        }
        s += "}\n";
    }
    return s;
}

std::string buildGlslFragmentShader(const RecompD3dVertexLayout &layout, uint32_t fvf,
                                    uint32_t program_count)
{
    const bool has_diffuse = layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT;
    const bool four_coords = (layout.texcoord_count == 4u);
    const bool two_coords = (layout.texcoord_count == 2u);

    std::string s;
    s.reserve(4096);
    s += "#version 450\n";
    s += kGlslUniformsFragment;
    s += "layout(set = 2, binding = 0) uniform sampler2D guest_texture;\n";
    s += "layout(set = 2, binding = 1) uniform sampler2D alpha_mask;\n";
    s += "layout(location = 0) in vec4 v_color;\n";
    s += "layout(location = 1) in vec2 v_texcoord;\n";
    s += "layout(location = 2) in vec2 v_reflection_coord;\n";
    if (four_coords) {
        s += "layout(location = 3) in vec2 v_texcoord1;\n";
        s += "layout(location = 4) in vec2 v_texcoord2;\n";
        s += "layout(location = 5) in vec2 v_texcoord3;\n";
    } else if (two_coords) {
        s += "layout(location = 3) in vec2 v_texcoord1;\n";
    }
    if (program_count) {
        s += "layout(location = 6) in vec2 v_program_q;\n";
    }

    s += "layout(location = 0) out vec4 out_color;\n";
    s += "void main() {\n";
    s += "    vec2 tc = v_texcoord;\n";
    s += "    vec2 refl_tc = v_reflection_coord;\n";
    if (program_count) {
        s += "    tc /= v_program_q.x;\n";
        s += "    if (u.reflection_flags.z > 0.5) refl_tc /= v_program_q.y;\n";
    }
    s += "    vec4 shaded = v_color;\n";
    s += "    if (u.reflection_flags.x > 0.5) {\n";
    s += "        vec4 base = texture(guest_texture, tc);\n";
    s += "        base.a *= u.reflection_diffuse.a;\n";
    s += "        vec4 env = texture(alpha_mask, refl_tc);\n";
    s += "        vec4 diffuse = u.reflection_diffuse;\n";
    s += "        if (u.directional_flags.x > 0.5) diffuse.rgb = v_color.rgb;\n";
    s += "        shaded = mix(base, env, env.a) * diffuse;\n";
    s += "    } else\n";
    if (four_coords) {
        s += "    if (u.texture_flags.z > 0.5) {\n";
        s += "        vec4 t0 = texture(guest_texture, tc * u.texture_flags.xy);\n";
        s += "        vec4 t1 = texture(guest_texture, v_texcoord1 * u.texture_flags.xy);\n";
        s += "        vec4 t2 = texture(guest_texture, v_texcoord2 * u.texture_flags.xy);\n";
        s += "        vec4 t3 = texture(guest_texture, v_texcoord3 * u.texture_flags.xy);\n";
        s += "        shaded = 0.5 * (clamp((128.0 / 255.0) * (t0 + t1), 0.0, 1.0) + clamp((128.0 / 255.0) * (t2 + t3), 0.0, 1.0));\n";
        s += "    } else\n";
    } else if (two_coords) {
        s += "    if (u.texture_flags.w > 0.5) {\n";
        s += "        vec3 rgb = texture(guest_texture, v_texcoord1 * u.texture_flags.xy).rgb;\n";
        s += "        float alpha = texture(alpha_mask, tc * u.lighting_flags.zw).a;\n";
        s += "        shaded = v_color * vec4(rgb, alpha);\n";
        s += "    } else\n";
    }
    s += "    if (u.blend_flags.y == 1.0) {\n";
    s += "        shaded = u.texture_factor;\n";
    s += "    } else if (u.draw_flags.x > 0.5) {\n";
    s += "        vec4 sampled = texture(guest_texture, tc * u.texture_flags.xy);\n";
    s += "        if (u.lighting_flags.y > 0.5) sampled.rgb = vec3(1.0);\n";
    s += has_diffuse ? "        shaded *= sampled;\n" : "        shaded = sampled;\n";
    s += "        if (u.directional_flags.x > 0.5) shaded.rgb *= v_color.rgb;\n";
    s += "        if (u.lighting_flags.x > 0.5) shaded.rgb = vec3(0.0);\n";
    s += "        shaded.a = (u.blend_flags.w > 0.5) ? u.blend_flags.z : (shaded.a * u.blend_flags.z);\n";
    s += "        if (u.blend_flags.y > 1.5) shaded *= u.texture_factor;\n";
    s += "    }\n";
    s += "    if (u.reflection_flags.z > 0.5) {\n";
    s += "        shaded.a *= texture(alpha_mask, refl_tc, u.reflection_flags.w).a;\n";
    s += "    }\n";
    s += "    if (u.draw_flags.y > 0.5) {\n";
    s += "        float alpha = round(clamp(shaded.a, 0.0, 1.0) * 255.0);\n";
    s += "        float ref = u.draw_flags.w;\n";
    s += "        int func = int(u.draw_flags.z);\n";
    s += "        bool alpha_pass = (func == 7) ||\n";
    s += "            (func == 1 && alpha < ref) ||\n";
    s += "            (func == 2 && alpha == ref) ||\n";
    s += "            (func == 3 && alpha <= ref) ||\n";
    s += "            (func == 4 && alpha > ref) ||\n";
    s += "            (func == 5 && alpha != ref) ||\n";
    s += "            (func == 6 && alpha >= ref);\n";
    s += "        if (!alpha_pass) discard;\n";
    s += "    }\n";
    s += "    out_color = shaded;\n";
    s += "}\n";

    return s;
}

#if !defined(_WIN32)
// Vulkan takes SPIR-V; the generated GLSL is compiled at runtime by shaderc,
// loaded on demand so the runner has no build-time dependency on it. SteamOS
// and most distributions ship libshaderc_shared.
typedef void *(*ShadercCompilerInitFn)();
typedef void *(*ShadercCompileOptionsInitFn)();
typedef void *(*ShadercCompileIntoSpvFn)(void *, const char *, size_t, int, const char *, const char *, void *);
typedef int (*ShadercResultStatusFn)(void *);
typedef const char *(*ShadercResultErrorFn)(void *);
typedef const char *(*ShadercResultBytesFn)(void *);
typedef size_t (*ShadercResultLengthFn)(void *);
typedef void (*ShadercResultReleaseFn)(void *);

static ShadercCompilerInitFn s_shaderc_init = nullptr;
static ShadercCompileOptionsInitFn s_shaderc_opt_init = nullptr;
static ShadercCompileIntoSpvFn s_shaderc_compile = nullptr;
static ShadercResultStatusFn s_shaderc_status = nullptr;
static ShadercResultErrorFn s_shaderc_error = nullptr;
static ShadercResultBytesFn s_shaderc_bytes = nullptr;
static ShadercResultLengthFn s_shaderc_length = nullptr;
static ShadercResultReleaseFn s_shaderc_res_release = nullptr;
static bool s_shaderc_checked = false;

static bool loadShaderc() {
    if (s_shaderc_checked) return s_shaderc_compile != nullptr;
    s_shaderc_checked = true;
    void *h = dlopen("libshaderc_shared.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libshaderc_shared.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) return false;
    s_shaderc_init = (ShadercCompilerInitFn)dlsym(h, "shaderc_compiler_initialize");
    s_shaderc_opt_init = (ShadercCompileOptionsInitFn)dlsym(h, "shaderc_compile_options_initialize");
    s_shaderc_compile = (ShadercCompileIntoSpvFn)dlsym(h, "shaderc_compile_into_spv");
    s_shaderc_status = (ShadercResultStatusFn)dlsym(h, "shaderc_result_get_compilation_status");
    s_shaderc_error = (ShadercResultErrorFn)dlsym(h, "shaderc_result_get_error_message");
    s_shaderc_bytes = (ShadercResultBytesFn)dlsym(h, "shaderc_result_get_bytes");
    s_shaderc_length = (ShadercResultLengthFn)dlsym(h, "shaderc_result_get_length");
    s_shaderc_res_release = (ShadercResultReleaseFn)dlsym(h, "shaderc_result_release");
    if (!s_shaderc_init || !s_shaderc_compile || !s_shaderc_status || !s_shaderc_bytes ||
        !s_shaderc_length || !s_shaderc_res_release) {
        s_shaderc_compile = nullptr;
        return false;
    }
    return true;
}
#endif

#if !defined(_WIN32)
static void *s_persistent_compiler = nullptr;
static void *s_persistent_options = nullptr;
static std::mutex s_compiler_mutex;

static std::vector<uint32_t> compileGlslToSpirv(const std::string &source, SDL_GPUShaderStage stage) {
    std::vector<uint32_t> spv;
    if (!loadShaderc()) {
        std::fprintf(stderr, "[presenter] Vulkan shaders need libshaderc_shared.so.1 (shaderc)\n");
        return spv;
    }
    std::lock_guard<std::mutex> lock(s_compiler_mutex);
    if (!s_persistent_compiler) {
        s_persistent_compiler = s_shaderc_init();
        if (s_shaderc_opt_init) {
            s_persistent_options = s_shaderc_opt_init();
        }
    }
    if (!s_persistent_compiler) return spv;
    const int kind = (stage == SDL_GPU_SHADERSTAGE_VERTEX) ? 0 : 1; // shaderc_vertex_shader, shaderc_fragment_shader
    void *result = s_shaderc_compile(s_persistent_compiler, source.c_str(), source.size(), kind, "main.glsl", "main", s_persistent_options);
    if (!result) return spv;
    if (s_shaderc_status(result) == 0) {
        const size_t len = s_shaderc_length(result);
        if (len >= 4 && (len % 4 == 0)) {
            spv.resize(len / 4);
            std::memcpy(spv.data(), s_shaderc_bytes(result), len);
        }
    } else if (s_shaderc_error) {
        std::fprintf(stderr, "[presenter] shaderc compilation failed: %s\n", s_shaderc_error(result));
    }
    s_shaderc_res_release(result);
    return spv;
}
#endif

ShaderPair getOrCreateShaderPair(RecompD3dPresenter *presenter, const RecompD3dVertexLayout &layout,
                                 uint32_t fvf, uint32_t program_count, const uint32_t (*program_tokens)[4])
{
    uint64_t shader_key = fvf ^ (static_cast<uint64_t>(program_count) << 32);
    if (program_count && program_tokens) {
        shader_key ^= programHash(program_tokens, program_count);
    }
    {
        std::lock_guard<std::mutex> lock(presenter->shader_mutex);
        auto it = presenter->shader_cache.find(shader_key);
        if (it != presenter->shader_cache.end()) {
            return it->second;
        }
    }

    if (presenter->use_spirv) {
        const std::vector<uint32_t> vs_spv = compileGlslToSpirv(
            buildGlslVertexShader(layout, fvf, program_count, program_tokens), SDL_GPU_SHADERSTAGE_VERTEX);
        const std::vector<uint32_t> fs_spv = compileGlslToSpirv(
            buildGlslFragmentShader(layout, fvf, program_count), SDL_GPU_SHADERSTAGE_FRAGMENT);
        if (vs_spv.empty() || fs_spv.empty()) {
            std::fprintf(stderr, "[presenter] failed to compile GLSL->SPIR-V shader fvf=0x%08X\n", fvf);
            return {nullptr, nullptr};
        }
        const uint32_t *vs_code = vs_spv.data();
        const size_t vs_size = vs_spv.size() * sizeof(uint32_t);
        const uint32_t *fs_code = fs_spv.data();
        const size_t fs_size = fs_spv.size() * sizeof(uint32_t);

        SDL_GPUShaderCreateInfo vs_info{};
        vs_info.code_size = vs_size;
        vs_info.code = reinterpret_cast<const Uint8 *>(vs_code);
        vs_info.entrypoint = "main";
        vs_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        vs_info.num_uniform_buffers = program_count ? 2 : 1;
        SDL_GPUShader *vs = SDL_CreateGPUShader(presenter->device, &vs_info);

        SDL_GPUShaderCreateInfo fs_info{};
        fs_info.code_size = fs_size;
        fs_info.code = reinterpret_cast<const Uint8 *>(fs_code);
        fs_info.entrypoint = "main";
        fs_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        fs_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fs_info.num_samplers = 2;
        fs_info.num_uniform_buffers = 1;
        SDL_GPUShader *fs = SDL_CreateGPUShader(presenter->device, &fs_info);

        if (!vs || !fs) {
            std::fprintf(stderr, "[presenter] failed to create SPIR-V shader fvf=0x%08X: %s\n", fvf, SDL_GetError());
            if (vs) SDL_ReleaseGPUShader(presenter->device, vs);
            if (fs) SDL_ReleaseGPUShader(presenter->device, fs);
            return {nullptr, nullptr};
        }

        ShaderPair pair{vs, fs};
        {
            std::lock_guard<std::mutex> lock(presenter->shader_mutex);
            presenter->shader_cache[shader_key] = pair;
        }
        return pair;
    }

    std::string msl = buildMslShader(layout, fvf, program_count, program_tokens);
    SDL_GPUShaderCreateInfo vs_info{};
    vs_info.code_size = msl.size();
    vs_info.code = reinterpret_cast<const Uint8 *>(msl.c_str());
    vs_info.entrypoint = "vs_main";
    vs_info.format = SDL_GPU_SHADERFORMAT_MSL;
    vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vs_info.num_uniform_buffers = program_count ? 2 : 1;
    SDL_GPUShader *vs = SDL_CreateGPUShader(presenter->device, &vs_info);

    SDL_GPUShaderCreateInfo fs_info{};
    fs_info.code_size = msl.size();
    fs_info.code = reinterpret_cast<const Uint8 *>(msl.c_str());
    fs_info.entrypoint = "ps_main";
    fs_info.format = SDL_GPU_SHADERFORMAT_MSL;
    fs_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fs_info.num_samplers = 2;
    fs_info.num_uniform_buffers = 1;
    SDL_GPUShader *fs = SDL_CreateGPUShader(presenter->device, &fs_info);

    if (!vs || !fs) {
        std::fprintf(stderr, "[presenter] failed to compile MSL shader fvf=0x%08X: %s\n", fvf, SDL_GetError());
        if (vs) SDL_ReleaseGPUShader(presenter->device, vs);
        if (fs) SDL_ReleaseGPUShader(presenter->device, fs);
        return {nullptr, nullptr};
    }

    ShaderPair pair{vs, fs};
    {
        std::lock_guard<std::mutex> lock(presenter->shader_mutex);
        presenter->shader_cache[shader_key] = pair;
    }
    return pair;
}

void precompileBootShaders(RecompD3dPresenter *p)
{
    for (const uint32_t fvf : kBootDrawFvfs) {
        if (p->precompile_stop.load()) return;
        RecompD3dVertexLayout layout{};
        if (!recomp_d3d_fvf_layout(fvf, &layout)) continue;
        getOrCreateShaderPair(p, layout, fvf, 0u, nullptr);
    }
}

SDL_GPUGraphicsPipeline *getOrCreatePipeline(RecompD3dPresenter *presenter,
                                             const RecompD3dPresenterDrawCommand &draw,
                                             const RecompD3dVertexLayout &layout,
                                             SDL_GPUTextureFormat color_fmt,
                                             SDL_GPUTextureFormat depth_fmt,
                                             SDL_GPUSampleCount sample_count)
{
    const bool is_strip = (draw.primitive_type == RECOMP_D3D_PT_TRIANGLESTRIP);
    uint64_t prog_hash = draw.program_count ? programHash(draw.program, draw.program_count) : 0u;

    PipelineKey pkey{};
    pkey.fvf = draw.fvf;
    pkey.vertex_stride = draw.vertex_stride;
    pkey.program_count = draw.program_count;
    pkey.program_hash = prog_hash;
    pkey.is_strip = is_strip;
    pkey.cull_mode = draw.cull_mode;
    pkey.color_format = color_fmt;
    pkey.depth_format = depth_fmt;
    pkey.sample_count = sample_count;
    pkey.alpha_to_coverage = sample_count != SDL_GPU_SAMPLECOUNT_1 && draw.depth.alpha_test_enable;

    pkey.depth_test_enable = draw.depth.depth_test_enable;
    pkey.depth_write_enable = draw.depth.depth_write_enable;
    pkey.depth_func = (pkey.depth_test_enable || pkey.depth_write_enable)
        ? draw.depth.depth_func : RECOMP_D3D_COMPARE_LESS_EQUAL;

    pkey.stencil_enable = draw.depth.stencil_enable;
    if (pkey.stencil_enable) {
        pkey.stencil_read_mask = static_cast<uint8_t>(draw.depth.stencil_read_mask);
        pkey.stencil_write_mask = static_cast<uint8_t>(draw.depth.stencil_write_mask);
        pkey.stencil_func = draw.depth.stencil_func;
        pkey.stencil_fail = draw.depth.stencil_fail;
        pkey.stencil_zfail = draw.depth.stencil_zfail;
        pkey.stencil_pass = draw.depth.stencil_pass;
    } else {
        pkey.stencil_read_mask = 0xff;
        pkey.stencil_write_mask = 0xff;
        pkey.stencil_func = RECOMP_D3D_COMPARE_ALWAYS;
        pkey.stencil_fail = RECOMP_D3D_STENCIL_KEEP;
        pkey.stencil_zfail = RECOMP_D3D_STENCIL_KEEP;
        pkey.stencil_pass = RECOMP_D3D_STENCIL_KEEP;
    }

    pkey.blend_enable = draw.blend.blend_enable;
    pkey.color_write_mask = draw.blend.color_write_mask;
    if (pkey.blend_enable) {
        pkey.src_factor = draw.blend.src_factor;
        pkey.dst_factor = draw.blend.dst_factor;
        pkey.blend_op = draw.blend.op;
    } else {
        pkey.src_factor = RECOMP_D3D_BLEND_ONE;
        pkey.dst_factor = RECOMP_D3D_BLEND_ZERO;
        pkey.blend_op = RECOMP_D3D_BLEND_OP_ADD;
    }

    if (presenter->last_pipeline != nullptr && presenter->last_pkey == pkey) {
        return presenter->last_pipeline;
    }

    auto it = presenter->pipelines.find(pkey);
    if (it != presenter->pipelines.end()) {
        presenter->last_pkey = pkey;
        presenter->last_pipeline = it->second;
        return it->second;
    }

    ShaderPair shaders = getOrCreateShaderPair(presenter, layout, draw.fvf, draw.program_count,
                                               draw.program_count ? draw.program : nullptr);
    if (!shaders.vertex_shader || !shaders.fragment_shader) {
        return nullptr;
    }

    SDL_GPUGraphicsPipelineCreateInfo pinfo{};
    pinfo.vertex_shader = shaders.vertex_shader;
    pinfo.fragment_shader = shaders.fragment_shader;
    pinfo.primitive_type = is_strip ? SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pinfo.multisample_state.sample_count = sample_count;
    pinfo.multisample_state.enable_alpha_to_coverage = pkey.alpha_to_coverage;

    // Rasterizer state
    pinfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    if (draw.cull_mode == RECOMP_D3D_CULL_NONE) {
        pinfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pinfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    } else if (draw.cull_mode == RECOMP_D3D_CULL_CLOCKWISE) {
        pinfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
        pinfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    } else {
        pinfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
        pinfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    }

    // Depth-stencil state
    pinfo.depth_stencil_state.enable_depth_test = pkey.depth_test_enable;
    pinfo.depth_stencil_state.enable_depth_write = pkey.depth_write_enable;
    pinfo.depth_stencil_state.compare_op = hostCompareOp(pkey.depth_func);
    pinfo.depth_stencil_state.enable_stencil_test = pkey.stencil_enable;
    pinfo.depth_stencil_state.compare_mask = pkey.stencil_read_mask;
    pinfo.depth_stencil_state.write_mask = pkey.stencil_write_mask;
    pinfo.depth_stencil_state.front_stencil_state.fail_op = hostStencilOp(pkey.stencil_fail);
    pinfo.depth_stencil_state.front_stencil_state.pass_op = hostStencilOp(pkey.stencil_pass);
    pinfo.depth_stencil_state.front_stencil_state.depth_fail_op = hostStencilOp(pkey.stencil_zfail);
    pinfo.depth_stencil_state.front_stencil_state.compare_op = hostCompareOp(pkey.stencil_func);
    pinfo.depth_stencil_state.back_stencil_state = pinfo.depth_stencil_state.front_stencil_state;

    // Target info & blend
    SDL_GPUColorTargetDescription color_desc{};
    color_desc.format = color_fmt;
    color_desc.blend_state.enable_blend = pkey.blend_enable;
    color_desc.blend_state.src_color_blendfactor = hostBlendFactor(pkey.src_factor, false);
    color_desc.blend_state.dst_color_blendfactor = hostBlendFactor(pkey.dst_factor, false);
    color_desc.blend_state.color_blend_op = hostBlendOp(pkey.blend_op);
    color_desc.blend_state.src_alpha_blendfactor = hostBlendFactor(pkey.src_factor, true);
    color_desc.blend_state.dst_alpha_blendfactor = hostBlendFactor(pkey.dst_factor, true);
    color_desc.blend_state.alpha_blend_op = hostBlendOp(pkey.blend_op);
    color_desc.blend_state.color_write_mask = pkey.color_write_mask;
    color_desc.blend_state.enable_color_write_mask = true;

    pinfo.target_info.color_target_descriptions = &color_desc;
    pinfo.target_info.num_color_targets = 1;
    pinfo.target_info.depth_stencil_format = depth_fmt;
    pinfo.target_info.has_depth_stencil_target = (depth_fmt != SDL_GPU_TEXTUREFORMAT_INVALID);

    // Vertex input layout
    SDL_GPUVertexBufferDescription vb_desc{};
    vb_desc.slot = 0;
    vb_desc.pitch = draw.vertex_stride;
    vb_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attributes[8]{};
    Uint32 attr_count = 0;

    // Position (attr 0)
    attributes[attr_count].location = 0;
    attributes[attr_count].buffer_slot = 0;
    attributes[attr_count].format = layout.pretransformed ? SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4 : SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[attr_count].offset = layout.position_offset;
    ++attr_count;

    // Weights (attr 1)
    if (layout.blend_weight_count != 0u) {
        attributes[attr_count].location = 1;
        attributes[attr_count].buffer_slot = 0;
        attributes[attr_count].format = (layout.blend_weight_count == 1u) ? SDL_GPU_VERTEXELEMENTFORMAT_FLOAT :
            ((layout.blend_weight_count == 2u) ? SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2 : SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3);
        attributes[attr_count].offset = 12u;
        ++attr_count;
    }

    // Normal (attr 2)
    if (layout.normal_offset != RECOMP_D3D_FVF_ABSENT) {
        attributes[attr_count].location = 2;
        attributes[attr_count].buffer_slot = 0;
        attributes[attr_count].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[attr_count].offset = layout.normal_offset;
        ++attr_count;
    }

    // Diffuse (attr 3)
    if (layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT) {
        attributes[attr_count].location = 3;
        attributes[attr_count].buffer_slot = 0;
        attributes[attr_count].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
        attributes[attr_count].offset = layout.diffuse_offset;
        ++attr_count;
    }

    // Texcoords (attr 4..7)
    if (layout.texcoord_offset != RECOMP_D3D_FVF_ABSENT) {
        const uint32_t tc_count = (layout.texcoord_count == 4u) ? 4u : ((layout.texcoord_count == 2u) ? 2u : 1u);
        for (uint32_t i = 0u; i < tc_count; ++i) {
            attributes[attr_count].location = 4u + i;
            attributes[attr_count].buffer_slot = 0;
            attributes[attr_count].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
            attributes[attr_count].offset = layout.texcoord_offset + i * 8u;
            ++attr_count;
        }
    }

    pinfo.vertex_input_state.vertex_buffer_descriptions = &vb_desc;
    pinfo.vertex_input_state.num_vertex_buffers = 1;
    pinfo.vertex_input_state.vertex_attributes = attributes;
    pinfo.vertex_input_state.num_vertex_attributes = attr_count;

    SDL_GPUGraphicsPipeline *pipe = SDL_CreateGPUGraphicsPipeline(presenter->device, &pinfo);
    if (!pipe) {
        std::fprintf(stderr, "[presenter] failed to create pipeline fvf=0x%08X: %s\n", draw.fvf, SDL_GetError());
        return nullptr;
    }

    presenter->pipelines[pkey] = pipe;
    presenter->last_pkey = pkey;
    presenter->last_pipeline = pipe;
    return pipe;
}

void flushSegment(RecompD3dPresenter *presenter)
{
    DrawSegment &seg = presenter->segment;
    if (seg.draws.empty()) return;

    const uint32_t seg_v_bytes = static_cast<uint32_t>(seg.vertex_data.size());
    const uint32_t seg_i_bytes = static_cast<uint32_t>(seg.index_data.size());

    if (seg_v_bytes > presenter->draw_vertex_capacity ||
        seg_i_bytes > presenter->draw_index_capacity) {
        if (!ensureDynamicBuffers(presenter, seg_v_bytes, seg_i_bytes)) {
            seg.draws.clear();
            seg.vertex_data.clear();
            seg.index_data.clear();
            seg.uniform_data.clear();
            return;
        }
    }

    uint32_t seg_v_base = (presenter->draw_vertex_used + 255u) & ~255u;
    uint32_t seg_i_base = (presenter->draw_index_used + 255u) & ~255u;

    const bool wrap_v = (seg_v_base + seg_v_bytes > presenter->draw_vertex_capacity);
    const bool wrap_i = (seg_i_base + seg_i_bytes > presenter->draw_index_capacity);
    if (wrap_v) seg_v_base = 0u;
    if (wrap_i) seg_i_base = 0u;

    void *vmap = SDL_MapGPUTransferBuffer(presenter->device, presenter->draw_vertex_transfer, wrap_v);
    if (vmap) {
        std::memcpy(static_cast<uint8_t *>(vmap) + seg_v_base, seg.vertex_data.data(), seg_v_bytes);
        SDL_UnmapGPUTransferBuffer(presenter->device, presenter->draw_vertex_transfer);
    }
    void *imap = SDL_MapGPUTransferBuffer(presenter->device, presenter->draw_index_transfer, wrap_i);
    if (imap) {
        std::memcpy(static_cast<uint8_t *>(imap) + seg_i_base, seg.index_data.data(), seg_i_bytes);
        SDL_UnmapGPUTransferBuffer(presenter->device, presenter->draw_index_transfer);
    }

    SDL_GPUCommandBuffer *cmdbuf = getCommandBuffer(presenter);
    SDL_GPUCopyPass *cpass = SDL_BeginGPUCopyPass(cmdbuf);
    if (cpass) {
        SDL_GPUTransferBufferLocation vsrc{presenter->draw_vertex_transfer, seg_v_base};
        SDL_GPUBufferRegion vdst{presenter->draw_vertex_buffer, seg_v_base, seg_v_bytes};
        SDL_UploadToGPUBuffer(cpass, &vsrc, &vdst, wrap_v);

        SDL_GPUTransferBufferLocation isrc{presenter->draw_index_transfer, seg_i_base};
        SDL_GPUBufferRegion idst{presenter->draw_index_buffer, seg_i_base, seg_i_bytes};
        SDL_UploadToGPUBuffer(cpass, &isrc, &idst, wrap_i);
        SDL_EndGPUCopyPass(cpass);
    }

    const bool is_main = (seg.target_color == presenter->backbuffer_color);
    const bool use_depth = (seg.target_depth != nullptr);

    SDL_GPUColorTargetInfo color_info{};
    color_info.texture = seg.target_color;
    color_info.load_op = SDL_GPU_LOADOP_LOAD;
    color_info.store_op = SDL_GPU_STOREOP_STORE;
    if (is_main) {
        presenter->backbuffer_resolved = false;
    }

    SDL_GPUDepthStencilTargetInfo depth_info{};
    if (use_depth) {
        depth_info.texture = seg.target_depth;
        depth_info.load_op = SDL_GPU_LOADOP_LOAD;
        depth_info.store_op = SDL_GPU_STOREOP_STORE;
        depth_info.stencil_load_op = SDL_GPU_LOADOP_LOAD;
        depth_info.stencil_store_op = SDL_GPU_STOREOP_STORE;
    }

    SDL_GPURenderPass *rpass = SDL_BeginGPURenderPass(cmdbuf, &color_info, 1, use_depth ? &depth_info : nullptr);
    if (rpass) {
        SDL_GPUViewport viewport{0.0f, 0.0f, seg.target_w, seg.target_h, 0.0f, 1.0f};
        SDL_SetGPUViewport(rpass, &viewport);
        SDL_Rect scissor{0, 0, static_cast<int>(seg.target_w), static_cast<int>(seg.target_h)};
        SDL_SetGPUScissor(rpass, &scissor);

        SDL_GPUGraphicsPipeline *last_pipe = nullptr;
        uint32_t last_v_offset = UINT32_MAX;
        uint32_t last_i_offset = UINT32_MAX;
        uint32_t last_vs_offset = UINT32_MAX;
        uint32_t last_vs_size = 0;
        uint32_t last_fs_offset = UINT32_MAX;
        uint32_t last_program_offset = UINT32_MAX;
        uint32_t last_fs_size = 0;
        SDL_GPUTexture *last_t0 = nullptr;
        SDL_GPUSampler *last_s0 = nullptr;
        SDL_GPUTexture *last_t1 = nullptr;
        SDL_GPUSampler *last_s1 = nullptr;
        // Wider than the values they track, so the first draw always sets them.
        uint64_t last_constant_color = UINT64_MAX;
        int last_stencil_ref = -1;

        for (const QueuedDraw &q : seg.draws) {
            if (!q.pipeline) continue;

            if (q.pipeline != last_pipe) {
                SDL_BindGPUGraphicsPipeline(rpass, q.pipeline);
                last_pipe = q.pipeline;
            }

            const uint32_t cur_v_offset = seg_v_base + q.vertex_offset;
            if (cur_v_offset != last_v_offset) {
                SDL_GPUBufferBinding vbinding{presenter->draw_vertex_buffer, cur_v_offset};
                SDL_BindGPUVertexBuffers(rpass, 0, &vbinding, 1);
                last_v_offset = cur_v_offset;
            }

            const uint32_t cur_i_offset = seg_i_base + q.index_offset;
            if (cur_i_offset != last_i_offset) {
                SDL_GPUBufferBinding ibinding{presenter->draw_index_buffer, cur_i_offset};
                SDL_BindGPUIndexBuffer(rpass, &ibinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
                last_i_offset = cur_i_offset;
            }

            if (q.uniform_offset != last_vs_offset || q.vs_uniform_size != last_vs_size) {
                SDL_PushGPUVertexUniformData(cmdbuf, 0, &seg.uniform_data[q.uniform_offset], q.vs_uniform_size * sizeof(float));
                last_vs_offset = q.uniform_offset;
                last_vs_size = q.vs_uniform_size;
            }

            if (q.program_offset != kNoProgramConstants && q.program_offset != last_program_offset) {
                SDL_PushGPUVertexUniformData(cmdbuf, 1, &seg.uniform_data[q.program_offset], kProgramConstantFloats * sizeof(float));
                last_program_offset = q.program_offset;
            }

            if (q.uniform_offset != last_fs_offset || q.fs_uniform_size != last_fs_size) {
                SDL_PushGPUFragmentUniformData(cmdbuf, 0, &seg.uniform_data[q.uniform_offset], q.fs_uniform_size * sizeof(float));
                last_fs_offset = q.uniform_offset;
                last_fs_size = q.fs_uniform_size;
            }

            SDL_GPUTexture *t0 = q.texture ? q.texture : presenter->dummy_texture;
            SDL_GPUTexture *t1 = q.mask_texture ? q.mask_texture : presenter->dummy_texture;
            if (t0 != last_t0 || q.sampler0 != last_s0 || t1 != last_t1 || q.sampler1 != last_s1) {
                SDL_GPUTextureSamplerBinding samplers[2] = {
                    {t0, q.sampler0},
                    {t1, q.sampler1}
                };
                SDL_BindGPUFragmentSamplers(rpass, 0, samplers, 2);
                last_t0 = t0;
                last_s0 = q.sampler0;
                last_t1 = t1;
                last_s1 = q.sampler1;
            }

            if (q.constant_color != last_constant_color) {
                const uint32_t color = q.constant_color;
                SDL_FColor blend_factor = {
                    ((color >> 16u) & 255u) / 255.0f,
                    ((color >> 8u) & 255u) / 255.0f,
                    (color & 255u) / 255.0f,
                    ((color >> 24u) & 255u) / 255.0f
                };
                SDL_SetGPUBlendConstants(rpass, blend_factor);
                last_constant_color = color;
            }

            if (q.stencil_ref != last_stencil_ref) {
                SDL_SetGPUStencilReference(rpass, q.stencil_ref);
                last_stencil_ref = q.stencil_ref;
            }

            SDL_DrawGPUIndexedPrimitives(rpass, q.index_count, 1, 0, 0, 0);
        }
        SDL_EndGPURenderPass(rpass);
    }

    presenter->draw_vertex_used = seg_v_base + seg_v_bytes;
    presenter->draw_index_used = seg_i_base + seg_i_bytes;

    seg.draws.clear();
    seg.vertex_data.clear();
    seg.index_data.clear();
    seg.uniform_data.clear();
}

} // namespace

extern "C" {

RecompD3dPresenterError recomp_d3d_presenter_create(
    const RecompD3dPresenterConfig *config,
    RecompD3dPresenter **presenter)
{
    if (config == nullptr || presenter == nullptr) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    if (config->width == 0u || config->height == 0u ||
        config->color_format != RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM ||
        config->depth_format != RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    if (*presenter != nullptr || active_presenter != nullptr) {
        return RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED;
    }

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[presenter] SDL init video failed: %s\n", SDL_GetError());
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }

    SDL_GPUDevice *device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    if (!device) {
        std::fprintf(stderr, "[presenter] SDL create GPU device failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }

    float scale = 1.0f;
    if (const char *scale_env = std::getenv("RECOMP_D3D_SCALE")) {
        const float val = static_cast<float>(std::atof(scale_env));
        if (std::isfinite(val) && val >= 1.0f) {
            scale = std::clamp(val, 1.0f, 8.0f);
        }
    }
    const char *widescreen_env = std::getenv("RECOMP_D3D_WIDESCREEN");
    const bool widescreen = (widescreen_env == nullptr || std::strcmp(widescreen_env, "0") != 0);

    const uint32_t main_h = static_cast<uint32_t>(config->height * scale + 0.5f);
    const uint32_t main_w = (scale == 1.0f) ? config->width
        : (widescreen ? (main_h * 16u + 8u) / 9u : (main_h * 4u + 1u) / 3u);

    uint32_t win_w = main_w;
    uint32_t win_h = main_h;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
    if (mode && mode->w > 0 && mode->h > 0) {
        uint32_t max_w = static_cast<uint32_t>(mode->w);
        uint32_t max_h = static_cast<uint32_t>(mode->h > 80 ? mode->h - 80 : mode->h);
        if (win_w > max_w || win_h > max_h) {
            float aspect = static_cast<float>(main_w) / main_h;
            if (win_w > max_w) {
                win_w = max_w;
                win_h = static_cast<uint32_t>(win_w / aspect + 0.5f);
            }
            if (win_h > max_h) {
                win_h = max_h;
                win_w = static_cast<uint32_t>(win_h * aspect + 0.5f);
            }
        }
    }

    const bool headless = std::getenv("RECOMP_HEADLESS") != nullptr ||
                          std::getenv("DOAXBV_HEADLESS") != nullptr;
    // Steam sets these in Game Mode and on the Deck, where gamescope would
    // only letterbox a window.
    const bool fullscreen = std::getenv("SteamTenfoot") != nullptr ||
                            std::getenv("SteamDeck") != nullptr;
    SDL_WindowFlags win_flags = SDL_WINDOW_RESIZABLE;
    if (headless) {
        win_flags |= SDL_WINDOW_HIDDEN;
    } else if (fullscreen) {
        win_flags |= SDL_WINDOW_FULLSCREEN;
    }

    SDL_Window *window = SDL_CreateWindow(
        "DOAXBV Recomp", win_w, win_h, win_flags);
    if (!window || !SDL_ClaimWindowForGPUDevice(device, window)) {
        std::fprintf(stderr, "[presenter] SDL create/claim window failed: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_DestroyGPUDevice(device);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }

    SDL_GPUPresentMode present_mode = SDL_GPU_PRESENTMODE_VSYNC;
    if (preferMailbox(immediate_present_setting) &&
        SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX)) {
        present_mode = SDL_GPU_PRESENTMODE_MAILBOX;
    }
    SDL_SetGPUSwapchainParameters(
        device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present_mode);

    uint32_t msaa_count = 1u;
    SDL_GPUSampleCount sample_count = SDL_GPU_SAMPLECOUNT_1;
    if (const char *msaa_env = std::getenv("RECOMP_D3D_MSAA")) {
        const int requested = std::atoi(msaa_env);
        if (requested >= 8 && SDL_GPUTextureSupportsSampleCount(device, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_SAMPLECOUNT_8) &&
                              SDL_GPUTextureSupportsSampleCount(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_SAMPLECOUNT_8)) {
            msaa_count = 8u;
            sample_count = SDL_GPU_SAMPLECOUNT_8;
        } else if (requested >= 4 && SDL_GPUTextureSupportsSampleCount(device, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_SAMPLECOUNT_4) &&
                                     SDL_GPUTextureSupportsSampleCount(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_SAMPLECOUNT_4)) {
            msaa_count = 4u;
            sample_count = SDL_GPU_SAMPLECOUNT_4;
        } else if (requested >= 2 && SDL_GPUTextureSupportsSampleCount(device, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_SAMPLECOUNT_2) &&
                                     SDL_GPUTextureSupportsSampleCount(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_SAMPLECOUNT_2)) {
            msaa_count = 2u;
            sample_count = SDL_GPU_SAMPLECOUNT_2;
        }
    }

    // Create backbuffer textures
    SDL_GPUTextureCreateInfo color_info{};
    color_info.type = SDL_GPU_TEXTURETYPE_2D;
    color_info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    color_info.usage = (sample_count == SDL_GPU_SAMPLECOUNT_1)
        ? (SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)
        : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    color_info.width = main_w;
    color_info.height = main_h;
    color_info.layer_count_or_depth = 1;
    color_info.num_levels = 1;
    color_info.sample_count = sample_count;
    SDL_GPUTexture *backbuffer_color = SDL_CreateGPUTexture(device, &color_info);

    SDL_GPUTextureCreateInfo depth_info{};
    depth_info.type = SDL_GPU_TEXTURETYPE_2D;
    depth_info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    depth_info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depth_info.width = main_w;
    depth_info.height = main_h;
    depth_info.layer_count_or_depth = 1;
    depth_info.num_levels = 1;
    depth_info.sample_count = sample_count;
    SDL_GPUTexture *backbuffer_depth = SDL_CreateGPUTexture(device, &depth_info);

    SDL_GPUTexture *backbuffer_resolve = nullptr;
    if (sample_count == SDL_GPU_SAMPLECOUNT_1) {
        backbuffer_resolve = backbuffer_color;
    } else {
        SDL_GPUTextureCreateInfo resolve_info{};
        resolve_info.type = SDL_GPU_TEXTURETYPE_2D;
        resolve_info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        resolve_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        resolve_info.width = main_w;
        resolve_info.height = main_h;
        resolve_info.layer_count_or_depth = 1;
        resolve_info.num_levels = 1;
        resolve_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        backbuffer_resolve = SDL_CreateGPUTexture(device, &resolve_info);
    }

    SDL_GPUTextureCreateInfo copy_info{};
    copy_info.type = SDL_GPU_TEXTURETYPE_2D;
    copy_info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    copy_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    copy_info.width = main_w;
    copy_info.height = main_h;
    copy_info.layer_count_or_depth = 1;
    copy_info.num_levels = 1;
    SDL_GPUTexture *backbuffer_copy = SDL_CreateGPUTexture(device, &copy_info);
    SDL_GPUTexture *frontbuffer_copy = SDL_CreateGPUTexture(device, &copy_info);

    if (!backbuffer_color || !backbuffer_depth || !backbuffer_resolve || !backbuffer_copy || !frontbuffer_copy) {
        if (backbuffer_color) SDL_ReleaseGPUTexture(device, backbuffer_color);
        if (backbuffer_depth) SDL_ReleaseGPUTexture(device, backbuffer_depth);
        if (backbuffer_resolve && backbuffer_resolve != backbuffer_color) SDL_ReleaseGPUTexture(device, backbuffer_resolve);
        if (backbuffer_copy) SDL_ReleaseGPUTexture(device, backbuffer_copy);
        if (frontbuffer_copy) SDL_ReleaseGPUTexture(device, frontbuffer_copy);
        SDL_ReleaseWindowFromGPUDevice(device, window);
        SDL_DestroyWindow(window);
        SDL_DestroyGPUDevice(device);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }

    // Dummy texture (1x1 white)
    SDL_GPUTextureCreateInfo dummy_info{};
    dummy_info.type = SDL_GPU_TEXTURETYPE_2D;
    dummy_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    dummy_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    dummy_info.width = 1;
    dummy_info.height = 1;
    dummy_info.layer_count_or_depth = 1;
    dummy_info.num_levels = 1;
    SDL_GPUTexture *dummy_texture = SDL_CreateGPUTexture(device, &dummy_info);
    if (dummy_texture) {
        uint32_t white_pixel = 0xffffffffu;
        SDL_GPUTransferBufferCreateInfo tbuf_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, sizeof(white_pixel), 0};
        SDL_GPUTransferBuffer *tbuf = SDL_CreateGPUTransferBuffer(device, &tbuf_info);
        if (tbuf) {
            void *map = SDL_MapGPUTransferBuffer(device, tbuf, false);
            if (map) {
                std::memcpy(map, &white_pixel, sizeof(white_pixel));
                SDL_UnmapGPUTransferBuffer(device, tbuf);
                SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
                SDL_GPUCopyPass *cpass = SDL_BeginGPUCopyPass(cmd);
                if (cpass) {
                    SDL_GPUTextureTransferInfo src{tbuf, 0, 1, 1};
                    SDL_GPUTextureRegion dst{dummy_texture, 0, 0, 0, 0, 0, 1, 1, 1};
                    SDL_UploadToGPUTexture(cpass, &src, &dst, false);
                    SDL_EndGPUCopyPass(cpass);
                }
                SDL_SubmitGPUCommandBuffer(cmd);
            }
            SDL_ReleaseGPUTransferBuffer(device, tbuf);
        }
    }

    // Samplers
    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler *filter_sampler = SDL_CreateGPUSampler(device, &sampler_info);

    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    SDL_GPUSampler *program_mask_sampler = SDL_CreateGPUSampler(device, &sampler_info);

    sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler *point_sampler = SDL_CreateGPUSampler(device, &sampler_info);

    // Initial dynamic ring buffers
    SDL_GPUBufferCreateInfo vb_info{SDL_GPU_BUFFERUSAGE_VERTEX, kInitialVertexCapacity, 0};
    SDL_GPUBuffer *vb = SDL_CreateGPUBuffer(device, &vb_info);
    SDL_GPUBufferCreateInfo ib_info{SDL_GPU_BUFFERUSAGE_INDEX, kInitialIndexCapacity, 0};
    SDL_GPUBuffer *ib = SDL_CreateGPUBuffer(device, &ib_info);

    SDL_GPUTransferBufferCreateInfo vtb_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, kInitialVertexCapacity, 0};
    SDL_GPUTransferBuffer *vtb = SDL_CreateGPUTransferBuffer(device, &vtb_info);
    SDL_GPUTransferBufferCreateInfo itb_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, kInitialIndexCapacity, 0};
    SDL_GPUTransferBuffer *itb = SDL_CreateGPUTransferBuffer(device, &itb_info);

    if (!vb || !ib || !vtb || !itb) {
        if (vb) SDL_ReleaseGPUBuffer(device, vb);
        if (ib) SDL_ReleaseGPUBuffer(device, ib);
        if (vtb) SDL_ReleaseGPUTransferBuffer(device, vtb);
        if (itb) SDL_ReleaseGPUTransferBuffer(device, itb);
        if (filter_sampler) SDL_ReleaseGPUSampler(device, filter_sampler);
        if (point_sampler) SDL_ReleaseGPUSampler(device, point_sampler);
        if (program_mask_sampler) SDL_ReleaseGPUSampler(device, program_mask_sampler);
        if (dummy_texture) SDL_ReleaseGPUTexture(device, dummy_texture);
        SDL_ReleaseGPUTexture(device, backbuffer_color);
        SDL_ReleaseGPUTexture(device, backbuffer_depth);
        if (backbuffer_resolve && backbuffer_resolve != backbuffer_color) SDL_ReleaseGPUTexture(device, backbuffer_resolve);
        SDL_ReleaseGPUTexture(device, backbuffer_copy);
        SDL_ReleaseGPUTexture(device, frontbuffer_copy);
        SDL_ReleaseWindowFromGPUDevice(device, window);
        SDL_DestroyWindow(window);
        SDL_DestroyGPUDevice(device);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }

    auto *p = new (std::nothrow) RecompD3dPresenter();
    if (!p) {
        SDL_ReleaseGPUBuffer(device, vb);
        SDL_ReleaseGPUBuffer(device, ib);
        SDL_ReleaseGPUTransferBuffer(device, vtb);
        SDL_ReleaseGPUTransferBuffer(device, itb);
        SDL_ReleaseGPUSampler(device, filter_sampler);
        SDL_ReleaseGPUSampler(device, point_sampler);
        SDL_ReleaseGPUSampler(device, program_mask_sampler);
        if (dummy_texture) SDL_ReleaseGPUTexture(device, dummy_texture);
        SDL_ReleaseGPUTexture(device, backbuffer_color);
        SDL_ReleaseGPUTexture(device, backbuffer_depth);
        if (backbuffer_resolve && backbuffer_resolve != backbuffer_color) SDL_ReleaseGPUTexture(device, backbuffer_resolve);
        SDL_ReleaseGPUTexture(device, backbuffer_copy);
        SDL_ReleaseGPUTexture(device, frontbuffer_copy);
        SDL_ReleaseWindowFromGPUDevice(device, window);
        SDL_DestroyWindow(window);
        SDL_DestroyGPUDevice(device);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    }

    p->config = *config;
    p->scale = scale;
    p->widescreen = widescreen;
    p->use_spirv = (SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV) != 0;
    p->owner_thread = std::this_thread::get_id();
    const char *performance = std::getenv("RECOMP_PERF_COUNTER");
    p->performance_counter = performance != nullptr && std::strcmp(performance, "1") == 0;
    p->window = window;
    p->device = device;
    p->backbuffer_color = backbuffer_color;
    p->backbuffer_depth = backbuffer_depth;
    p->backbuffer_resolve = backbuffer_resolve;
    p->backbuffer_copy = backbuffer_copy;
    p->frontbuffer_copy = frontbuffer_copy;
    p->dummy_texture = dummy_texture;
    p->msaa_count = msaa_count;
    p->msaa_samples = sample_count;
    p->filter_sampler = filter_sampler;
    p->point_sampler = point_sampler;
    p->program_mask_sampler = program_mask_sampler;
    p->draw_vertex_buffer = vb;
    p->draw_index_buffer = ib;
    p->draw_vertex_transfer = vtb;
    p->draw_index_transfer = itb;

    std::fprintf(stderr, "[presenter] initialized scale=%.2f resolution=%ux%u window=%ux%u widescreen=%d msaa=%ux\n",
        scale, main_w, main_h, win_w, win_h, widescreen ? 1 : 0, msaa_count);

    try {
        p->precompile_thread = std::thread(precompileBootShaders, p);
    } catch (const std::system_error &) {
    }

    active_presenter = p;
    *presenter = p;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError recomp_d3d_presenter_submit(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterCommand *command)
{
    if (presenter == nullptr || presenter != active_presenter) {
        return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    }
    if (std::this_thread::get_id() != presenter->owner_thread) {
        return RECOMP_D3D_PRESENTER_WRONG_THREAD;
    }
    // Events are pumped once per present; pumping per draw costs a syscall
    // round trip on X11/Wayland for each of the game's ~1500 draws a frame.
    if (presenter->close_requested) {
        return RECOMP_D3D_PRESENTER_CLOSED;
    }
    if (command == nullptr) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }

    switch (command->type) {
    case RECOMP_D3D_PRESENTER_COMMAND_CLEAR: {
        const auto &clear = command->data.clear;
        if (clear.clear_depth && (!std::isfinite(clear.z) || clear.z < 0.0f || clear.z > 1.0f)) {
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }

        flushSegment(presenter);

        SDL_GPUTexture *target_color = presenter->backbuffer_color;
        uint32_t color_w = mainWidth(presenter);
        uint32_t color_h = mainHeight(presenter);

        if (clear.target.offscreen) {
            color_w = clear.target.color.width;
            color_h = clear.target.color.height;
            RenderTargetEntry *rt = findRenderTarget(presenter, clear.target.color);
            if (!rt) {
                const uint64_t bytes = static_cast<uint64_t>(color_w) * color_h * 4u;
                if (bytes > kTargetByteLimit - presenter->target_bytes) {
                    return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
                }
                SDL_GPUTextureCreateInfo rt_info{};
                rt_info.type = SDL_GPU_TEXTURETYPE_2D;
                rt_info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
                rt_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
                rt_info.width = color_w;
                rt_info.height = color_h;
                rt_info.layer_count_or_depth = 1;
                rt_info.num_levels = 1;
                SDL_GPUTexture *new_rt = SDL_CreateGPUTexture(presenter->device, &rt_info);
                if (!new_rt) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
                presenter->render_targets.push_back({clear.target.color, new_rt, color_w, color_h});
                presenter->target_bytes += bytes;
                rt = &presenter->render_targets.back();
            }
            target_color = rt->texture;
        }
        SDL_GPUTexture *target_depth = lookupDepthTarget(presenter, clear.target, color_w, color_h);

        SDL_GPUCommandBuffer *cmdbuf = getCommandBuffer(presenter);
        SDL_GPUColorTargetInfo color_info{};
        color_info.texture = target_color;
        const bool is_main = (target_color == presenter->backbuffer_color);
        color_info.store_op = SDL_GPU_STOREOP_STORE;
        if (is_main) {
            presenter->backbuffer_resolved = false;
        }
        if (clear.clear_color) {
            color_info.load_op = SDL_GPU_LOADOP_CLEAR;
            constexpr float b2f = 1.0f / 255.0f;
            color_info.clear_color.r = static_cast<float>((clear.color >> 16u) & 0xffu) * b2f;
            color_info.clear_color.g = static_cast<float>((clear.color >> 8u) & 0xffu) * b2f;
            color_info.clear_color.b = static_cast<float>(clear.color & 0xffu) * b2f;
            color_info.clear_color.a = static_cast<float>((clear.color >> 24u) & 0xffu) * b2f;
        } else {
            color_info.load_op = SDL_GPU_LOADOP_LOAD;
        }

        SDL_GPUDepthStencilTargetInfo depth_info{};
        const bool use_depth = (target_depth != nullptr) && (clear.clear_depth || clear.clear_stencil);
        if (use_depth) {
            depth_info.texture = target_depth;
            depth_info.load_op = clear.clear_depth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            depth_info.store_op = SDL_GPU_STOREOP_STORE;
            depth_info.clear_depth = clear.z;
            depth_info.stencil_load_op = clear.clear_stencil ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            depth_info.stencil_store_op = SDL_GPU_STOREOP_STORE;
            depth_info.clear_stencil = static_cast<Uint8>(clear.stencil);
        }

        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmdbuf, &color_info, 1, use_depth ? &depth_info : nullptr);
        if (pass) SDL_EndGPURenderPass(pass);
        return RECOMP_D3D_PRESENTER_OK;
    }

    case RECOMP_D3D_PRESENTER_COMMAND_PRESENT: {
        const auto &present = command->data.present;
        if (present.effective_flags != 5u || present.swap_counter == 0u) {
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }

        flushSegment(presenter);
        ensureBackbufferResolved(presenter);

        pumpEvents(presenter);
        if (presenter->close_requested) {
            return RECOMP_D3D_PRESENTER_CLOSED;
        }

        SDL_GPUCommandBuffer *cmdbuf = getCommandBuffer(presenter);
        SDL_GPUTexture *swapchain_tex = nullptr;
        Uint32 swap_w = 0, swap_h = 0;
        if (SDL_WaitAndAcquireGPUSwapchainTexture(
                cmdbuf, presenter->window, &swapchain_tex, &swap_w, &swap_h)) {
            if (swapchain_tex && swap_w > 0 && swap_h > 0) {
                SDL_GPUBlitInfo blit{};
                blit.source.texture = presenter->backbuffer_resolve;
                blit.source.w = mainWidth(presenter);
                blit.source.h = mainHeight(presenter);
                blit.destination.texture = swapchain_tex;

                const float src_aspect = static_cast<float>(mainWidth(presenter)) / mainHeight(presenter);
                const float dst_aspect = static_cast<float>(swap_w) / swap_h;
                int dst_x = 0, dst_y = 0, dst_w = swap_w, dst_h = swap_h;
                if (dst_aspect > src_aspect) {
                    dst_w = static_cast<int>(swap_h * src_aspect + 0.5f);
                    dst_x = (swap_w - dst_w) / 2;
                } else {
                    dst_h = static_cast<int>(swap_w / src_aspect + 0.5f);
                    dst_y = (swap_h - dst_h) / 2;
                }

                blit.destination.x = dst_x;
                blit.destination.y = dst_y;
                blit.destination.w = dst_w;
                blit.destination.h = dst_h;
                blit.load_op = SDL_GPU_LOADOP_CLEAR;
                blit.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
                blit.filter = SDL_GPU_FILTER_LINEAR;
                SDL_BlitGPUTexture(cmdbuf, &blit);
            }
        }

        if (presenter->frontbuffer_copy) {
            SDL_GPUCopyPass *copypass = SDL_BeginGPUCopyPass(cmdbuf);
            if (copypass) {
                SDL_GPUTextureLocation src_loc{presenter->backbuffer_resolve, 0, 0, 0, 0};
                SDL_GPUTextureLocation dst_loc{presenter->frontbuffer_copy, 0, 0, 0, 0};
                SDL_CopyGPUTextureToTexture(copypass, &src_loc, &dst_loc, mainWidth(presenter), mainHeight(presenter), 1, false);
                SDL_EndGPUCopyPass(copypass);
            }
        }

        SDL_SubmitGPUCommandBuffer(cmdbuf);
        presenter->current_command_buffer = nullptr;
        ++presenter->present_count;

        if (presenter->performance_counter) reportPerformance(presenter);

        return RECOMP_D3D_PRESENTER_OK;
    }

    case RECOMP_D3D_PRESENTER_COMMAND_DRAW: {
        const auto &draw = command->data.draw;
        if (draw.vertex_bytes == nullptr || draw.index_bytes == nullptr ||
            draw.vertex_stride == 0u || draw.vertex_count == 0u ||
            draw.index_count == 0u ||
            static_cast<unsigned>(draw.cull_mode) > RECOMP_D3D_CULL_COUNTER_CLOCKWISE) {
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }

        RecompD3dVertexLayout layout{};
        if (!recomp_d3d_fvf_layout(draw.fvf, &layout) ||
            layout.stride > draw.vertex_stride ||
            (!layout.pretransformed && !draw.has_transform)) {
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }

        // Determine destination target
        SDL_GPUTexture *target_color = presenter->backbuffer_color;
        uint32_t target_color_data = 0;
        uint32_t color_w = mainWidth(presenter);
        uint32_t color_h = mainHeight(presenter);

        if (draw.target.offscreen) {
            target_color_data = draw.target.color.data;
            color_w = draw.target.color.width;
            color_h = draw.target.color.height;
            RenderTargetEntry *rt = findRenderTarget(presenter, draw.target.color);
            if (!rt) {
                const uint64_t bytes = static_cast<uint64_t>(color_w) * color_h * 4u;
                if (bytes <= kTargetByteLimit - presenter->target_bytes) {
                    SDL_GPUTextureCreateInfo rt_info{};
                    rt_info.type = SDL_GPU_TEXTURETYPE_2D;
                    rt_info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
                    rt_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
                    rt_info.width = color_w;
                    rt_info.height = color_h;
                    rt_info.layer_count_or_depth = 1;
                    rt_info.num_levels = 1;
                    SDL_GPUTexture *new_rt = SDL_CreateGPUTexture(presenter->device, &rt_info);
                    if (new_rt) {
                        presenter->render_targets.push_back({draw.target.color, new_rt, color_w, color_h});
                        presenter->target_bytes += bytes;
                        rt = &presenter->render_targets.back();
                    }
                }
            }
            if (rt && rt->texture) {
                target_color = rt->texture;
            }
        }

        SDL_GPUTexture *target_depth = lookupDepthTarget(presenter, draw.target, color_w, color_h);
        float target_w = static_cast<float>(color_w);
        float target_h = static_cast<float>(color_h);

        // Target switch or sampling current render target requires flushing active segment
        if (presenter->segment.target_color != target_color ||
            presenter->segment.target_depth != target_depth ||
            (draw.has_texture && (draw.texture_is_backbuffer || (draw.texture.data == presenter->segment.target_color_data && presenter->segment.target_color_data != 0u)))) {
            flushSegment(presenter);
        }

        presenter->segment.target_color = target_color;
        presenter->segment.target_depth = target_depth;
        presenter->segment.target_color_data = target_color_data;
        presenter->segment.target_w = target_w;
        presenter->segment.target_h = target_h;

        // Prepare textures
        SDL_GPUTexture *texture = draw.has_texture ? prepareTexture(presenter, draw) : nullptr;
        SDL_GPUTexture *mask_texture = nullptr;

        if (draw.has_alpha_mask) {
            RecompD3dPresenterDrawCommand mask_cmd{};
            mask_cmd.has_texture = true;
            mask_cmd.texture = draw.alpha_mask;
            mask_cmd.texture_bytes = draw.alpha_mask_bytes;
            mask_cmd.texture_byte_count = draw.alpha_mask_byte_count;
            mask_cmd.palette_bytes = draw.alpha_mask_palette;
            mask_cmd.palette_byte_count = draw.alpha_mask_palette_byte_count;
            mask_texture = prepareTexture(presenter, mask_cmd);
        } else if (draw.has_reflection || draw.program_alpha_mask) {
            RecompD3dPresenterDrawCommand refl_cmd{};
            refl_cmd.has_texture = true;
            refl_cmd.texture = draw.reflection_texture;
            refl_cmd.texture_bytes = draw.reflection_bytes;
            refl_cmd.texture_byte_count = draw.reflection_byte_count;
            mask_texture = prepareTexture(presenter, refl_cmd);
        }

        // Append vertex data
        auto &vdata = presenter->segment.vertex_data;
        uint32_t v_offset = static_cast<uint32_t>(vdata.size());
        if (v_offset % 16u != 0u) {
            vdata.resize((v_offset + 15u) & ~15u, 0);
            v_offset = static_cast<uint32_t>(vdata.size());
        }
        const uint32_t v_size = draw.vertex_stride * draw.vertex_count;
        const auto *v_src = static_cast<const uint8_t *>(draw.vertex_bytes);
        vdata.insert(vdata.end(), v_src, v_src + v_size);

        // Append index data
        auto &idata = presenter->segment.index_data;
        uint32_t i_offset = static_cast<uint32_t>(idata.size());
        if (i_offset % 16u != 0u) {
            idata.resize((i_offset + 15u) & ~15u, 0);
            i_offset = static_cast<uint32_t>(idata.size());
        }

        uint32_t queued_index_count = draw.index_count;
        if (draw.primitive_type == RECOMP_D3D_PT_TRIANGLEFAN) {
            const auto *src_idx = static_cast<const uint16_t *>(draw.index_bytes);
            queued_index_count = draw.triangle_count * 3u;
            for (uint32_t t = 0u; t < draw.triangle_count; ++t) {
                uint16_t fan[3] = {src_idx[0], src_idx[t + 1u], src_idx[t + 2u]};
                const auto *b = reinterpret_cast<const uint8_t *>(fan);
                idata.insert(idata.end(), b, b + sizeof(fan));
            }
        } else {
            const uint32_t i_size = draw.index_count * sizeof(uint16_t);
            const auto *i_src = static_cast<const uint8_t *>(draw.index_bytes);
            idata.insert(idata.end(), i_src, i_src + i_size);
        }
        const uint32_t i_size = queued_index_count * sizeof(uint16_t);

        const bool is_main = (target_color == presenter->backbuffer_color);
        const bool use_msaa = is_main && (presenter->msaa_samples != SDL_GPU_SAMPLECOUNT_1);
        const SDL_GPUSampleCount pass_samples = use_msaa ? presenter->msaa_samples : SDL_GPU_SAMPLECOUNT_1;
        const SDL_GPUTextureFormat color_fmt = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        const bool use_depth = (target_depth != nullptr);
        const SDL_GPUTextureFormat depth_fmt = use_depth ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT : SDL_GPU_TEXTUREFORMAT_INVALID;

        // A draw whose shader or pipeline failed is skipped, as before.
        SDL_GPUGraphicsPipeline *pipeline = getOrCreatePipeline(presenter, draw, layout, color_fmt, depth_fmt, pass_samples);
        if (!pipeline) {
            return RECOMP_D3D_PRESENTER_OK;
        }

        SDL_GPUSampler *s0 = (draw.four_tap_filter || draw.has_alpha_mask || draw.program_count)
            ? presenter->filter_sampler
            : lookupDrawSampler(presenter, draw.address_u, draw.address_v);
        SDL_GPUSampler *s1 = draw.program_alpha_mask ? presenter->program_mask_sampler : presenter->filter_sampler;

        float constants[kTransformUniformFloats]{};

        // Setup draw constants matching D3D11 layout
        std::memcpy(constants, draw.transform, sizeof(draw.transform));
        std::memcpy(constants + 16, draw.blend_transforms, sizeof(draw.blend_transforms));

        if (layout.pretransformed || draw.program_count) {
            std::memset(constants, 0, sizeof(draw.transform));
            const float guest_w = draw.target.offscreen ? target_w : static_cast<float>(presenter->config.width);
            const float guest_h = draw.target.offscreen ? target_h : static_cast<float>(presenter->config.height);
            constants[0] = 2.0f / guest_w;
            constants[5] = -2.0f / guest_h;
            constants[10] = 1.0f;
            constants[12] = -1.0f + 0.5f * constants[0];
            constants[13] = 1.0f + 0.5f * constants[5];
            constants[14] = 0.0f;
            constants[15] = 1.0f;
        }

        if (draw.program_count) {
            for (unsigned axis = 0; axis < 2; ++axis) {
                const float scale = draw.program_constants[58][axis];
                if (std::isfinite(scale) && scale != 0.0f) {
                    constants[axis * 5] = 1.0f / scale;
                    constants[12 + axis] = -draw.program_constants[59][axis] / scale;
                }
            }
            const float depth_scale = draw.program_constants[58][2];
            if (std::isfinite(depth_scale) && depth_scale > 0.0f) {
                constants[10] = 1.0f / depth_scale;
                constants[14] = -draw.program_constants[59][2] / depth_scale;
            }
            constants[138] = draw.program_alpha_mask ? 1.0f : 0.0f;
            constants[139] = draw.program_mask_lod_bias;
        }

        if (draw.directional.enabled && !draw.program_count) {
            const auto &light = draw.directional;
            float *dir_constants = constants + 144;
            std::memcpy(dir_constants, light.normal_transforms, sizeof(light.normal_transforms));
            std::memcpy(dir_constants + 64, light.ambient_emissive, sizeof(light.ambient_emissive));
            std::memcpy(dir_constants + 68, light.material_diffuse, sizeof(light.material_diffuse));
            std::memcpy(dir_constants + 72, light.directions, sizeof(light.directions));
            std::memcpy(dir_constants + 104, light.colors, sizeof(light.colors));
            constants[140] = 1.0f;
            constants[141] = light.normalize ? 1.0f : 0.0f;
            constants[142] = static_cast<float>(light.count);
        }

        constants[64] = (texture != nullptr) ? 1.0f : 0.0f;
        constants[65] = draw.depth.alpha_test_enable ? 1.0f : 0.0f;
        constants[66] = static_cast<float>(draw.depth.alpha_func);
        constants[67] = static_cast<float>(draw.depth.alpha_ref);
        constants[68] = (draw.blend_weight_count != 0u) ? 1.0f : 0.0f;
        constants[69] = draw.use_texture_factor ? 1.0f : (draw.modulate_texture_factor ? 2.0f : 0.0f);
        constants[70] = (draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_NONE) ? 1.0f : draw.material_alpha;
        constants[71] = (draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE) ? 1.0f : 0.0f;

        constants[72] = ((draw.texture_factor >> 16u) & 0xffu) / 255.0f;
        constants[73] = ((draw.texture_factor >> 8u) & 0xffu) / 255.0f;
        constants[74] = (draw.texture_factor & 0xffu) / 255.0f;
        constants[75] = ((draw.texture_factor >> 24u) & 0xffu) / 255.0f;

        constants[76] = draw.zero_diffuse_rgb ? 1.0f : 0.0f;
        constants[77] = (draw.texture.format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8) ? 1.0f : 0.0f;
        constants[78] = (draw.alpha_mask.linear && draw.alpha_mask.width) ? 1.0f / draw.alpha_mask.width : 1.0f;
        constants[79] = (draw.alpha_mask.linear && draw.alpha_mask.height) ? 1.0f / draw.alpha_mask.height : 1.0f;
        constants[80] = (draw.texture.linear && draw.texture.width) ? 1.0f / draw.texture.width : 1.0f;
        constants[81] = (draw.texture.linear && draw.texture.height) ? 1.0f / draw.texture.height : 1.0f;
        constants[82] = draw.four_tap_filter ? 1.0f : 0.0f;
        constants[83] = draw.has_alpha_mask ? 1.0f : 0.0f;

        if (draw.has_reflection) {
            std::memcpy(constants + 84, draw.reflection_world_view, 64u);
            std::memcpy(constants + 100, draw.reflection_normal, 64u);
            std::memcpy(constants + 116, draw.reflection_transform, 64u);
            std::memcpy(constants + 132, draw.reflection_diffuse, 16u);
            constants[136] = draw.reflection_mesh_uv ? 2.0f : 1.0f;
            constants[137] = draw.reflection_normalize ? 1.0f : 0.0f;
        }

        // Only lit draws push the directional block; vertex programs push
        // their constants to the second vertex uniform slot.
        const uint32_t vs_uniform_size = (draw.directional.enabled && !draw.program_count)
            ? kTransformUniformFloats : kHeaderUniformFloats;
        const uint32_t fs_uniform_size = kHeaderUniformFloats;

        auto &udata = presenter->segment.uniform_data;
        uint32_t uniform_offset = 0;
        bool reused_uniforms = false;
        if (!presenter->segment.draws.empty()) {
            const auto &prev = presenter->segment.draws.back();
            if (prev.vs_uniform_size == vs_uniform_size &&
                prev.fs_uniform_size == fs_uniform_size &&
                prev.uniform_offset + vs_uniform_size <= udata.size() &&
                std::memcmp(constants, &udata[prev.uniform_offset], vs_uniform_size * sizeof(float)) == 0) {
                uniform_offset = prev.uniform_offset;
                reused_uniforms = true;
            }
        }

        if (!reused_uniforms) {
            uniform_offset = static_cast<uint32_t>(udata.size());
            udata.insert(udata.end(), constants, constants + vs_uniform_size);
        }
        uint32_t program_offset = kNoProgramConstants;
        if (draw.program_count) {
            program_offset = static_cast<uint32_t>(udata.size());
            const float *program_constants = &draw.program_constants[0][0];
            udata.insert(udata.end(), program_constants, program_constants + kProgramConstantFloats);
        }

        QueuedDraw q{};
        q.pipeline = pipeline;
        q.vertex_offset = v_offset;
        q.vertex_size = v_size;
        q.index_offset = i_offset;
        q.index_size = i_size;
        q.index_count = queued_index_count;
        q.texture = texture;
        q.mask_texture = mask_texture;
        q.sampler0 = s0;
        q.sampler1 = s1;
        q.constant_color = draw.blend.constant_color;
        q.stencil_ref = static_cast<uint8_t>(draw.depth.stencil_ref);
        q.uniform_offset = uniform_offset;
        q.vs_uniform_size = vs_uniform_size;
        q.program_offset = program_offset;
        q.fs_uniform_size = fs_uniform_size;

        presenter->segment.draws.push_back(q);
        ++presenter->draw_count;
        return RECOMP_D3D_PRESENTER_OK;
    }

    case RECOMP_D3D_PRESENTER_COMMAND_GAMMA:
        return RECOMP_D3D_PRESENTER_OK;

    default:
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
}

RecompD3dPresenterError recomp_d3d_presenter_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size)
{
    if (presenter == nullptr || presenter != active_presenter) {
        return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    }
    if (std::this_thread::get_id() != presenter->owner_thread) {
        return RECOMP_D3D_PRESENTER_WRONG_THREAD;
    }
    flushSegment(presenter);

    if (base >= 0x80000000u && base < 0x84000000u) base -= 0x80000000u;
    if (size == 0u || static_cast<uint64_t>(base) + size > UINT64_C(0x100000000)) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }

    const auto released = [&](uint32_t data) {
        if (data >= 0x80000000u && data < 0x84000000u) data -= 0x80000000u;
        return static_cast<uint64_t>(data) >= static_cast<uint64_t>(base) &&
               static_cast<uint64_t>(data) < static_cast<uint64_t>(base) + static_cast<uint64_t>(size);
    };

    for (uint32_t i = 0u; i < presenter->texture_count; ++i) {
        TextureEntry &entry = presenter->textures[i];
        if (entry.used && released(entry.data)) {
            unindexTexture(presenter, i);
            if (entry.texture) SDL_ReleaseGPUTexture(presenter->device, entry.texture);
            entry.used = false;
        }
    }
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError recomp_d3d_presenter_destroy(
    RecompD3dPresenter **presenter)
{
    if (presenter == nullptr) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    if (*presenter == nullptr || *presenter != active_presenter) {
        return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    }
    if (std::this_thread::get_id() != (*presenter)->owner_thread) {
        return RECOMP_D3D_PRESENTER_WRONG_THREAD;
    }

    auto *p = *presenter;
    p->precompile_stop = true;
    if (p->precompile_thread.joinable()) {
        p->precompile_thread.join();
    }
    flushSegment(p);

    if (p->current_command_buffer) {
        SDL_SubmitGPUCommandBuffer(p->current_command_buffer);
        p->current_command_buffer = nullptr;
    }

    // Release cached textures
    for (uint32_t i = 0u; i < p->texture_count; ++i) {
        if (p->textures[i].used && p->textures[i].texture) {
            SDL_ReleaseGPUTexture(p->device, p->textures[i].texture);
        }
    }
    p->textures.clear();
    p->texture_index.clear();

    // Release render targets
    for (auto &rt : p->render_targets) {
        if (rt.texture) SDL_ReleaseGPUTexture(p->device, rt.texture);
    }
    p->render_targets.clear();

    // Release depth targets
    for (auto &dt : p->depth_targets) {
        if (dt.texture) SDL_ReleaseGPUTexture(p->device, dt.texture);
    }
    p->depth_targets.clear();

    // Release pipelines
    for (auto &pair : p->pipelines) {
        if (pair.second) SDL_ReleaseGPUGraphicsPipeline(p->device, pair.second);
    }
    p->pipelines.clear();

    // Release compiled shaders
    for (auto &pair : p->shader_cache) {
        if (pair.second.vertex_shader) SDL_ReleaseGPUShader(p->device, pair.second.vertex_shader);
        if (pair.second.fragment_shader) SDL_ReleaseGPUShader(p->device, pair.second.fragment_shader);
    }
    p->shader_cache.clear();

    // Release dynamic ring buffers
    if (p->draw_vertex_buffer) SDL_ReleaseGPUBuffer(p->device, p->draw_vertex_buffer);
    if (p->draw_index_buffer) SDL_ReleaseGPUBuffer(p->device, p->draw_index_buffer);
    if (p->draw_vertex_transfer) SDL_ReleaseGPUTransferBuffer(p->device, p->draw_vertex_transfer);
    if (p->draw_index_transfer) SDL_ReleaseGPUTransferBuffer(p->device, p->draw_index_transfer);
    p->draw_vertex_used = 0u;
    p->draw_index_used = 0u;

    // Release samplers
    for (auto &row : p->address_samplers) {
        for (auto &s : row) {
            if (s) {
                SDL_ReleaseGPUSampler(p->device, s);
                s = nullptr;
            }
        }
    }
    if (p->filter_sampler) SDL_ReleaseGPUSampler(p->device, p->filter_sampler);
    if (p->point_sampler) SDL_ReleaseGPUSampler(p->device, p->point_sampler);
    if (p->program_mask_sampler) SDL_ReleaseGPUSampler(p->device, p->program_mask_sampler);
    if (p->dummy_texture) SDL_ReleaseGPUTexture(p->device, p->dummy_texture);
    if (p->backbuffer_color) SDL_ReleaseGPUTexture(p->device, p->backbuffer_color);
    if (p->backbuffer_depth) SDL_ReleaseGPUTexture(p->device, p->backbuffer_depth);
    if (p->backbuffer_resolve && p->backbuffer_resolve != p->backbuffer_color) SDL_ReleaseGPUTexture(p->device, p->backbuffer_resolve);
    if (p->backbuffer_copy) SDL_ReleaseGPUTexture(p->device, p->backbuffer_copy);
    if (p->frontbuffer_copy) SDL_ReleaseGPUTexture(p->device, p->frontbuffer_copy);

    if (p->window) {
        SDL_ReleaseWindowFromGPUDevice(p->device, p->window);
        SDL_DestroyWindow(p->window);
    }
    if (p->device) {
        SDL_DestroyGPUDevice(p->device);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    delete p;
    active_presenter = nullptr;
    *presenter = nullptr;
    return RECOMP_D3D_PRESENTER_OK;
}

void recomp_d3d_presenter_set_immediate_present(bool enabled)
{
    immediate_present_setting = enabled;
    if (active_presenter && active_presenter->window && active_presenter->device) {
        const auto mode = preferMailbox(enabled) &&
            SDL_WindowSupportsGPUPresentMode(
                active_presenter->device, active_presenter->window, SDL_GPU_PRESENTMODE_MAILBOX)
            ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC;
        SDL_SetGPUSwapchainParameters(
            active_presenter->device, active_presenter->window,
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
    }
}

bool recomp_d3d_presenter_split_enabled(void)
{
    return false;
}

void recomp_d3d_presenter_report_draw_textures(void)
{
}

} // extern "C"
