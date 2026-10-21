// SPDX-License-Identifier: GPL-3.0-or-later
// OpenGL 4.3 core presenter, ported from d3d_presenter_sdl_gpu.cpp for the
// Switch. glClipControl(UPPER_LEFT, ZERO_TO_ONE) keeps the D3D clip space,
// depth range and top-row-first targets that presenter relies on, so its
// uniforms and shaders carry over unchanged.
#include "d3d_presenter.h"
#include "d3d_draw_model.h"
#include "d3d_glsl_shader.h"
#include "d3d_texture_model.h"

#include <glad/glad.h>

#ifdef __SWITCH__
#include <EGL/egl.h>
#include <switch.h>
#else
#error "d3d_presenter_gl.cpp has a host window only for the Switch (EGL on libnx)"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// devkitPro's glad loads GL 4.3 core with no extensions.
#ifndef GL_UPPER_LEFT
#define GL_UPPER_LEFT 0x8CA2
#endif
#ifndef GL_ZERO_TO_ONE
#define GL_ZERO_TO_ONE 0x935F
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

// shortcut: no MSL path, shaderc/SPIR-V or boot-shader precompile thread; GL
// contexts are per thread, so shaders compile lazily on the submit thread.
// Add a shared-context precompile thread if first-use hitches matter.
// shortcut: no MSAA (RECOMP_D3D_MSAA); add multisample renderbuffers and a
// resolve blit if a player asks for it.

namespace {

constexpr uint32_t kTextureSlots = 4096u;
constexpr uint32_t kPaletteBytes = 256u * 4u;
constexpr uint64_t kTargetByteLimit = 64u * 1024u * 1024u;
constexpr uint32_t kInitialVertexCapacity = 32u * 1024u * 1024u; // 32 MB
constexpr uint32_t kInitialIndexCapacity = 16u * 1024u * 1024u;  // 16 MB
constexpr uint32_t kInitialUniformCapacity = 16u * 1024u * 1024u; // 16 MB
constexpr uint32_t kStreamAlignment = 256u;
// TransformUniforms in floats: the header every draw uses, then the
// directional block. Vertex program constants have their own block.
constexpr uint32_t kHeaderUniformFloats = 144u;
constexpr uint32_t kTransformUniformFloats = 280u;
constexpr uint32_t kProgramConstantFloats = 192u * 4u;
constexpr uint32_t kNoProgramConstants = UINT32_MAX;

typedef void (APIENTRYP ClipControlFn)(GLenum origin, GLenum depth);

struct TextureEntry {
    bool used = false;
    uint32_t data = 0;
    uint32_t format_byte = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 0;
    uint8_t palette[kPaletteBytes]{};
    uint64_t fingerprint = 0;
    GLuint texture = 0;
};

struct RenderTargetEntry {
    RecompD3dTextureDesc desc{};
    GLuint texture = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct DepthTargetEntry {
    RecompD3dTextureDesc desc{};
    GLuint renderbuffer = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

GLenum hostBlendFactor(RecompD3dBlendFactor factor, bool alpha_channel)
{
    switch (factor) {
    case RECOMP_D3D_BLEND_ZERO: return GL_ZERO;
    case RECOMP_D3D_BLEND_ONE: return GL_ONE;
    case RECOMP_D3D_BLEND_SRC_COLOR:
        return alpha_channel ? GL_SRC_ALPHA : GL_SRC_COLOR;
    case RECOMP_D3D_BLEND_INV_SRC_COLOR:
        return alpha_channel ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE_MINUS_SRC_COLOR;
    case RECOMP_D3D_BLEND_SRC_ALPHA: return GL_SRC_ALPHA;
    case RECOMP_D3D_BLEND_INV_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case RECOMP_D3D_BLEND_DST_ALPHA: return GL_DST_ALPHA;
    case RECOMP_D3D_BLEND_INV_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case RECOMP_D3D_BLEND_DST_COLOR:
        return alpha_channel ? GL_DST_ALPHA : GL_DST_COLOR;
    case RECOMP_D3D_BLEND_INV_DST_COLOR:
        return alpha_channel ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_DST_COLOR;
    case RECOMP_D3D_BLEND_SRC_ALPHA_SATURATE: return GL_SRC_ALPHA_SATURATE;
    case RECOMP_D3D_BLEND_CONSTANT_COLOR: return GL_CONSTANT_COLOR;
    case RECOMP_D3D_BLEND_INV_CONSTANT_COLOR: return GL_ONE_MINUS_CONSTANT_COLOR;
    default: return GL_ONE;
    }
}

GLenum hostBlendOp(RecompD3dBlendOp op)
{
    switch (op) {
    case RECOMP_D3D_BLEND_OP_MIN: return GL_MIN;
    case RECOMP_D3D_BLEND_OP_MAX: return GL_MAX;
    case RECOMP_D3D_BLEND_OP_SUBTRACT: return GL_FUNC_SUBTRACT;
    case RECOMP_D3D_BLEND_OP_REVERSE_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
    case RECOMP_D3D_BLEND_OP_ADD:
    default: return GL_FUNC_ADD;
    }
}

GLenum hostCompareOp(RecompD3dCompareFunc func)
{
    switch (func) {
    case RECOMP_D3D_COMPARE_NEVER: return GL_NEVER;
    case RECOMP_D3D_COMPARE_LESS: return GL_LESS;
    case RECOMP_D3D_COMPARE_EQUAL: return GL_EQUAL;
    case RECOMP_D3D_COMPARE_LESS_EQUAL: return GL_LEQUAL;
    case RECOMP_D3D_COMPARE_GREATER: return GL_GREATER;
    case RECOMP_D3D_COMPARE_NOT_EQUAL: return GL_NOTEQUAL;
    case RECOMP_D3D_COMPARE_GREATER_EQUAL: return GL_GEQUAL;
    case RECOMP_D3D_COMPARE_ALWAYS: return GL_ALWAYS;
    default: return GL_LEQUAL;
    }
}

GLenum hostStencilOp(RecompD3dStencilOp op)
{
    switch (op) {
    case RECOMP_D3D_STENCIL_KEEP: return GL_KEEP;
    case RECOMP_D3D_STENCIL_ZERO: return GL_ZERO;
    case RECOMP_D3D_STENCIL_REPLACE: return GL_REPLACE;
    case RECOMP_D3D_STENCIL_INCRSAT: return GL_INCR;
    case RECOMP_D3D_STENCIL_DECRSAT: return GL_DECR;
    case RECOMP_D3D_STENCIL_INVERT: return GL_INVERT;
    case RECOMP_D3D_STENCIL_INCRWRAP: return GL_INCR_WRAP;
    case RECOMP_D3D_STENCIL_DECRWRAP: return GL_DECR_WRAP;
    default: return GL_KEEP;
    }
}

GLenum hostAddressMode(uint32_t mode)
{
    switch (mode) {
    case 1: return GL_REPEAT;          // WRAP
    case 2: return GL_MIRRORED_REPEAT; // MIRROR
    case 3: return GL_CLAMP_TO_EDGE;   // CLAMP
    case 4: return GL_CLAMP_TO_EDGE;   // BORDER
    case 5: return GL_CLAMP_TO_EDGE;   // CLAMPTOEDGE
    default: return GL_REPEAT;
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

// Vertex programs key shaders, so every token counts; the sampled texture
// fingerprint would let two programs share one shader.
uint64_t programHash(const uint32_t (*tokens)[4], uint32_t count)
{
    const auto *data = reinterpret_cast<const uint8_t *>(tokens);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t i = 0u; i < count * 16u; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}

struct HostTextureFormat {
    GLenum internal_format = 0; // zero: no upload path for this format
    GLenum format = 0;
    GLenum type = 0;
    bool compressed = false;
};

// The one place each guest format's upload is chosen. Mesa's S3TC upload
// reportedly corrupted the heap in another Switch DOAXBV port; a CPU DXT
// decode would map DXT1/3/5 to the BGRA8 entry here and decode before upload.
HostTextureFormat hostTextureFormat(uint32_t format_byte)
{
    switch (format_byte) {
    case RECOMP_D3D_TEXTURE_FORMAT_DXT1: return {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, true};
    case RECOMP_D3D_TEXTURE_FORMAT_DXT3: return {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, true};
    case RECOMP_D3D_TEXTURE_FORMAT_DXT5: return {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, true};
    case RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8:
    case RECOMP_D3D_TEXTURE_FORMAT_P8: return {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, false};
    case RECOMP_D3D_TEXTURE_FORMAT_A8: return {GL_R8, GL_RED, GL_UNSIGNED_BYTE, false};
    default: return {};
    }
}

bool isSwizzledTextureFormat(uint32_t format_byte)
{
    return format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8 ||
           format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8;
}

// The raster, depth-stencil and blend state SDL bakes into a pipeline.
struct DrawState {
    RecompD3dCullMode cull_mode = RECOMP_D3D_CULL_NONE;

    bool depth_test_enable = false;
    bool depth_write_enable = false;
    RecompD3dCompareFunc depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
    bool stencil_enable = false;
    uint8_t stencil_ref = 0;
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
    uint32_t constant_color = 0;

    bool operator==(const DrawState &o) const {
        return cull_mode == o.cull_mode &&
               depth_test_enable == o.depth_test_enable &&
               depth_write_enable == o.depth_write_enable &&
               depth_func == o.depth_func &&
               stencil_enable == o.stencil_enable &&
               stencil_ref == o.stencil_ref &&
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
               blend_op == o.blend_op &&
               constant_color == o.constant_color;
    }
};

struct QueuedDraw {
    GLuint program = 0;
    GLuint vao = 0;
    GLenum mode = GL_TRIANGLES;
    DrawState state{};
    uint32_t vertex_offset = 0;
    uint32_t vertex_stride = 0;
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    GLuint texture = 0;
    GLuint mask_texture = 0;
    GLuint sampler0 = 0;
    GLuint sampler1 = 0;
    uint32_t uniform_offset = 0;
    uint32_t vs_uniform_size = 0;
    uint32_t program_offset = 0;
};

struct DrawSegment {
    GLuint target_color = 0;
    GLuint target_depth = 0;
    uint32_t target_color_data = 0;
    uint32_t target_w = 0;
    uint32_t target_h = 0;
    std::vector<uint8_t> vertex_data;
    std::vector<uint8_t> index_data;
    std::vector<float> uniform_data;
    std::vector<QueuedDraw> draws;
};

// A linked program and the vertex array that feeds its FVF.
struct ShaderProgram {
    GLuint program = 0;
    GLuint vao = 0;
};

} // namespace

struct RecompD3dPresenter {
    RecompD3dPresenterConfig config{};
    std::thread::id owner_thread;
#ifdef __SWITCH__
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    Framebuffer window{};
    bool window_ready = false;
    GLuint window_texture = 0;
    GLuint window_framebuffer = 0;
    GLuint readback[2]{}; // pixel-pack buffers, used in turn
    uint32_t readback_frames = 0;
#endif

    GLuint backbuffer_color = 0;
    GLuint backbuffer_depth = 0; // renderbuffer
    GLuint backbuffer_copy = 0;
    GLuint frontbuffer_copy = 0;
    GLuint dummy_texture = 0;
    GLuint present_program = 0;
    GLuint present_vao = 0;

    // Persistent samplers
    GLuint filter_sampler = 0;
    GLuint program_mask_sampler = 0;
    GLuint address_samplers[6][6]{}; // [address_u][address_v]

    // Streaming rings; a wrap orphans the buffer so queued GPU reads keep their data.
    GLuint draw_vertex_buffer = 0;
    GLuint draw_index_buffer = 0;
    GLuint draw_uniform_buffer = 0;
    uint32_t draw_vertex_capacity = kInitialVertexCapacity;
    uint32_t draw_index_capacity = kInitialIndexCapacity;
    uint32_t draw_uniform_capacity = kInitialUniformCapacity;
    uint32_t draw_vertex_used = 0u;
    uint32_t draw_index_used = 0u;
    uint32_t draw_uniform_used = 0u;
    uint32_t uniform_align_floats = kStreamAlignment / 4u;

    // Current segment batch
    DrawSegment segment;

    // Program and framebuffer caches
    std::unordered_map<uint64_t, ShaderProgram> shader_cache;
    std::unordered_map<uint64_t, GLuint> framebuffers; // color << 32 | depth

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
    double second_submit_ms = 0.0; // inside submit, including the swap
    double second_swap_ms = 0.0;
    double second_flush_ms = 0.0; // issuing queued draws to GL

    float scale = 1.0f;
    bool widescreen = true;
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

#ifdef __SWITCH__
// Provided by switch_runner.c: releases the libnx text console that owns the window.
extern "C" void recomp_switch_console_exit(void);
#endif

namespace {

RecompD3dPresenter *active_presenter = nullptr;
bool immediate_present_setting = false;

// ── Host window: a surfaceless EGL context, read back into a libnx framebuffer ──
//
// In Eden 0.2.1 a frame never reaches the screen once a draw or blit has
// written Mesa's EGL window surface (a clear alone works), so frames are
// composed offscreen and copied to the window by the CPU.
// shortcut: one glReadPixels stall per frame; try an EGL window surface again
// on real hardware or a newer Eden.

#ifdef __SWITCH__
// 16:9 at the guest's 480 lines; the compositor scales it to the screen, so
// the per-frame readback and copy move half the bytes of 1280x720.
constexpr int kWindowWidth = 848, kWindowHeight = 480;

bool hostCreate(RecompD3dPresenter *p)
{
    static const EGLint config_attributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    static const EGLint context_attributes[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    recomp_switch_console_exit();
    p->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (p->display == EGL_NO_DISPLAY || !eglInitialize(p->display, nullptr, nullptr) ||
        !eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(p->display, config_attributes, &config, 1, &count) || count == 0 ||
        (p->context = eglCreateContext(p->display, config, EGL_NO_CONTEXT, context_attributes)) == EGL_NO_CONTEXT ||
        !eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, p->context) || !gladLoadGL()) {
        std::fprintf(stderr, "[presenter] EGL setup failed 0x%x\n", static_cast<unsigned>(eglGetError()));
        return false;
    }
    if (R_FAILED(framebufferCreate(&p->window, nwindowGetDefault(), kWindowWidth, kWindowHeight,
            PIXEL_FORMAT_RGBA_8888, 2)) || R_FAILED(framebufferMakeLinear(&p->window))) {
        std::fprintf(stderr, "[presenter] window framebuffer setup failed\n");
        return false;
    }
    p->window_ready = true;
    glGenTextures(1, &p->window_texture);
    glBindTexture(GL_TEXTURE_2D, p->window_texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kWindowWidth, kWindowHeight);
    glGenFramebuffers(1, &p->window_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, p->window_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p->window_texture, 0);
    glGenBuffers(2, p->readback);
    for (GLuint buffer : p->readback) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
        glBufferData(GL_PIXEL_PACK_BUFFER, kWindowWidth * kWindowHeight * 4, nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return true;
}

void hostDestroy(RecompD3dPresenter *p)
{
    if (p->window_ready) framebufferClose(&p->window);
    p->window_ready = false;
    if (p->display == EGL_NO_DISPLAY) return;
    glDeleteFramebuffers(1, &p->window_framebuffer);
    glDeleteTextures(1, &p->window_texture);
    glDeleteBuffers(2, p->readback);
    eglMakeCurrent(p->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (p->context != EGL_NO_CONTEXT) eglDestroyContext(p->display, p->context);
    eglTerminate(p->display);
    p->display = EGL_NO_DISPLAY;
    p->context = EGL_NO_CONTEXT;
}

// GL 4.5 / ARB_clip_control: outside the GL 4.3 set glad loads.
ClipControlFn hostClipControl()
{
    return reinterpret_cast<ClipControlFn>(eglGetProcAddress("glClipControl"));
}

// The framebuffer the present pass draws into, and its size.
GLuint hostWindowTarget(RecompD3dPresenter *p, int *width, int *height)
{
    *width = kWindowWidth;
    *height = kWindowHeight;
    return p->window_framebuffer;
}

// Each frame is read into one pixel-pack buffer while the previous frame's
// buffer, finished by now, goes to the screen: the CPU never waits for the
// GPU to finish the frame it just submitted, at one frame of extra latency.
// framebufferEnd queues the frame and waits for a free buffer, which paces
// presents to the display.
// shortcut: ignores recomp_d3d_presenter_set_immediate_present.
void hostSwap(RecompD3dPresenter *p)
{
    constexpr size_t kRowBytes = kWindowWidth * 4;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, p->window_framebuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, p->readback[p->readback_frames & 1u]);
    glReadPixels(0, 0, kWindowWidth, kWindowHeight, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFlush();
    if (p->readback_frames++ == 0u) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, p->readback[p->readback_frames & 1u]);
    const auto *source = static_cast<const uint8_t *>(
        glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, kRowBytes * kWindowHeight, GL_MAP_READ_BIT));
    u32 stride = 0;
    auto *pixels = static_cast<uint8_t *>(framebufferBegin(&p->window, &stride));
    if (source) {
        for (int y = 0; y < kWindowHeight; ++y) {
            std::memcpy(pixels + static_cast<size_t>(y) * stride, source + y * kRowBytes, kRowBytes);
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    framebufferEnd(&p->window);
}

void pumpEvents(RecompD3dPresenter *presenter)
{
    if (!appletMainLoop()) presenter->close_requested = true;
}
#endif

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
    std::fprintf(stderr, "recomp performance: present=%u fps=%.2f frame_ms=%.3f max_ms=%.1f draws=%u"
        " presenter_ms=%.1f flush_ms=%.1f swap_ms=%.1f\n",
        presenter->present_count, frames * 1000.0 / elapsed_ms, elapsed_ms / frames,
        presenter->second_max_ms, presenter->draw_count / frames,
        (presenter->second_submit_ms - presenter->second_swap_ms) / frames,
        presenter->second_flush_ms / frames,
        presenter->second_swap_ms / frames);
    presenter->second_presents = 1u;
    presenter->second_start = now;
    presenter->second_max_ms = 0.0;
    presenter->second_submit_ms = 0.0;
    presenter->second_swap_ms = 0.0;
    presenter->second_flush_ms = 0.0;
    presenter->draw_count = 0u;
}

// RECOMP_GL_DEBUG=1: Mesa's error and warning messages, each id once.
void APIENTRY reportGlMessage(GLenum source, GLenum type, GLuint id, GLenum severity,
    GLsizei, const GLchar *message, const void *)
{
    static std::vector<GLuint> seen;
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION || seen.size() >= 256u ||
        std::find(seen.begin(), seen.end(), id) != seen.end()) return;
    seen.push_back(id);
    std::fprintf(stderr, "[presenter] GL debug source=0x%x type=0x%x id=%u severity=0x%x: %s\n",
        source, type, id, severity, message);
}

void flushSegment(RecompD3dPresenter *presenter);

GLuint createTargetTexture(uint32_t width, uint32_t height)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    return texture;
}

GLuint createSampler(GLenum min_filter, GLenum wrap_u, GLenum wrap_v)
{
    GLuint sampler = 0;
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(min_filter));
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrap_u));
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrap_v));
    return sampler;
}

GLuint createStreamBuffer(uint32_t capacity)
{
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
    glBufferData(GL_COPY_WRITE_BUFFER, capacity, nullptr, GL_STREAM_DRAW);
    return buffer;
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

// Queued draws may still name the texture, so issue them before deleting it.
void releaseTexture(RecompD3dPresenter *presenter, uint32_t slot)
{
    flushSegment(presenter);
    TextureEntry &entry = presenter->textures[slot];
    unindexTexture(presenter, slot);
    glDeleteTextures(1, &entry.texture);
    entry.texture = 0;
    entry.used = false;
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

RenderTargetEntry *ensureRenderTarget(RecompD3dPresenter *presenter, const RecompD3dTextureDesc &desc)
{
    RenderTargetEntry *rt = findRenderTarget(presenter, desc);
    if (rt) return rt;
    const uint64_t bytes = static_cast<uint64_t>(desc.width) * desc.height * 4u;
    if (desc.width == 0u || desc.height == 0u || bytes > kTargetByteLimit - presenter->target_bytes) {
        return nullptr;
    }
    presenter->render_targets.push_back(
        {desc, createTargetTexture(desc.width, desc.height), desc.width, desc.height});
    presenter->target_bytes += bytes;
    return &presenter->render_targets.back();
}

GLuint lookupDepthTarget(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterTarget &target,
    uint32_t color_width,
    uint32_t color_height)
{
    if (target.no_depth) {
        return 0;
    }
    if (!target.custom_depth) {
        if (target.offscreen) return 0;
        return (color_width == mainWidth(presenter) && color_height == mainHeight(presenter))
            ? presenter->backbuffer_depth : 0;
    }

    const uint32_t width = target.depth.width;
    const uint32_t height = target.depth.height;

    if (width != color_width || height != color_height) {
        return 0;
    }

    const RecompD3dTextureDesc &desc = target.depth;
    if (desc.data == 0u || width == 0u || height == 0u) {
        return 0;
    }

    for (const auto &entry : presenter->depth_targets) {
        if (entry.desc.data == desc.data && entry.width == width && entry.height == height) {
            return entry.renderbuffer;
        }
    }

    const uint64_t bytes = static_cast<uint64_t>(width) * height * 4u;
    if (bytes > kTargetByteLimit - presenter->target_bytes) {
        return 0;
    }

    GLuint renderbuffer = 0;
    glGenRenderbuffers(1, &renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
        static_cast<GLsizei>(width), static_cast<GLsizei>(height));

    presenter->depth_targets.push_back({desc, renderbuffer, width, height});
    presenter->target_bytes += bytes;
    return renderbuffer;
}

GLuint framebufferFor(RecompD3dPresenter *presenter, GLuint color, GLuint depth)
{
    const uint64_t key = (static_cast<uint64_t>(color) << 32) | depth;
    const auto it = presenter->framebuffers.find(key);
    if (it != presenter->framebuffers.end()) return it->second;

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    if (depth) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    }
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[presenter] framebuffer incomplete 0x%x\n", static_cast<unsigned>(status));
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        fbo = 0; // cached, so a broken pair is not rebuilt on every draw
    }
    presenter->framebuffers[key] = fbo;
    return fbo;
}

bool bindTarget(RecompD3dPresenter *presenter, GLuint color, GLuint depth, uint32_t width, uint32_t height)
{
    const GLuint fbo = framebufferFor(presenter, color, depth);
    if (!fbo) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    return true;
}

GLuint lookupDrawSampler(RecompD3dPresenter *presenter, uint32_t address_u, uint32_t address_v)
{
    uint32_t u = address_u <= 5u ? address_u : 1u;
    uint32_t v = address_v <= 5u ? address_v : 1u;
    if (presenter->address_samplers[u][v] == 0) {
        presenter->address_samplers[u][v] =
            createSampler(GL_LINEAR_MIPMAP_LINEAR, hostAddressMode(u), hostAddressMode(v));
    }
    return presenter->address_samplers[u][v];
}

bool growStreamBuffer(GLuint buffer, uint32_t &capacity, uint32_t &used, uint32_t required)
{
    if (required <= capacity) return true;
    const uint32_t new_cap = std::max(required * 2u, capacity * 2u);
    glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
    glBufferData(GL_COPY_WRITE_BUFFER, new_cap, nullptr, GL_STREAM_DRAW);
    if (glGetError() != GL_NO_ERROR) return false;
    capacity = new_cap;
    used = 0u;
    return true;
}

bool ensureDynamicBuffers(RecompD3dPresenter *presenter, uint32_t req_vbytes, uint32_t req_ibytes, uint32_t req_ubytes)
{
    return growStreamBuffer(presenter->draw_vertex_buffer, presenter->draw_vertex_capacity,
                            presenter->draw_vertex_used, req_vbytes) &&
           growStreamBuffer(presenter->draw_index_buffer, presenter->draw_index_capacity,
                            presenter->draw_index_used, req_ibytes) &&
           growStreamBuffer(presenter->draw_uniform_buffer, presenter->draw_uniform_capacity,
                            presenter->draw_uniform_used, req_ubytes);
}

// GL_COPY_WRITE_BUFFER leaves the bound vertex array's element buffer alone.
uint32_t streamUpload(GLuint buffer, uint32_t capacity, uint32_t &used, const void *data, uint32_t bytes)
{
    glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
    uint32_t base = (used + kStreamAlignment - 1u) & ~(kStreamAlignment - 1u);
    if (base + bytes > capacity) {
        glBufferData(GL_COPY_WRITE_BUFFER, capacity, nullptr, GL_STREAM_DRAW);
        base = 0u;
    }
    void *map = glMapBufferRange(GL_COPY_WRITE_BUFFER, base, bytes,
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    if (map) {
        std::memcpy(map, data, bytes);
        glUnmapBuffer(GL_COPY_WRITE_BUFFER);
    }
    used = base + bytes;
    return base;
}

// Linear BGRA rows sit pitch bytes apart in guest memory.
void uploadLinearBgra(GLuint texture, const RecompD3dTextureDesc &desc, const void *bytes)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(desc.pitch / 4u));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(desc.width),
        static_cast<GLsizei>(desc.height), GL_BGRA, GL_UNSIGNED_BYTE, bytes);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

GLuint prepareTexture(RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand &draw)
{
    const RecompD3dTextureDesc &desc = draw.texture;
    if (!draw.has_texture || desc.data == 0u || desc.width == 0u || desc.height == 0u) {
        return 0;
    }
    if (draw.texture_is_backbuffer) {
        glCopyImageSubData(presenter->backbuffer_color, GL_TEXTURE_2D, 0, 0, 0, 0,
                           presenter->backbuffer_copy, GL_TEXTURE_2D, 0, 0, 0, 0,
                           static_cast<GLsizei>(mainWidth(presenter)),
                           static_cast<GLsizei>(mainHeight(presenter)), 1);
        return presenter->backbuffer_copy;
    }
    if (draw.texture_is_frontbuffer) {
        return presenter->frontbuffer_copy;
    }

    RenderTargetEntry *rt = findRenderTarget(presenter, desc);
    if (rt && rt->texture) {
        return rt->texture;
    }

    const bool palettized = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_P8;
    const bool linear_bgra = desc.format_byte == 0x12u;
    const HostTextureFormat format = hostTextureFormat(
        linear_bgra ? static_cast<uint32_t>(RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8) : desc.format_byte);
    const uint32_t levels = format.compressed && desc.mip_levels ? desc.mip_levels : 1u;

    if (palettized && (draw.palette_bytes == nullptr || draw.palette_byte_count != kPaletteBytes)) {
        return 0;
    }
    // GL reads whole rows straight from guest memory.
    if (linear_bgra && (desc.pitch % 4u != 0u || desc.pitch < desc.width * 4u ||
        static_cast<uint64_t>(desc.pitch) * (desc.height - 1u) + desc.width * 4u > draw.texture_byte_count)) {
        return 0;
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
                releaseTexture(presenter, it->second);
                break;
            }
            if (linear_bgra && draw.texture_bytes != nullptr && draw.texture_byte_count > 0u) {
                uploadLinearBgra(entry.texture, desc, draw.texture_bytes);
            }
            return entry.texture;
        }
    }

    if (format.internal_format == 0 || draw.texture_bytes == nullptr ||
        draw.texture_byte_count == 0u) {
        return 0;
    }

    std::vector<uint8_t> unswizzled;
    const auto *upload_bytes = static_cast<const uint8_t *>(draw.texture_bytes);
    uint32_t upload_size = draw.texture_byte_count;

    if (palettized) {
        const size_t texels = static_cast<size_t>(desc.width) * desc.height;
        if (texels > draw.texture_byte_count) return 0;
        std::vector<uint8_t> indices(texels);
        if (!recomp_d3d_texture_unswizzle(
                static_cast<const uint8_t *>(draw.texture_bytes),
                indices.data(), desc.width, desc.height, 1u)) {
            return 0;
        }
        unswizzled.resize(texels * 4u);
        const auto *palette = static_cast<const uint8_t *>(draw.palette_bytes);
        for (size_t i = 0u; i < texels; ++i) {
            std::memcpy(unswizzled.data() + i * 4u, palette + indices[i] * 4u, 4u);
        }
        upload_bytes = unswizzled.data();
        upload_size = static_cast<uint32_t>(unswizzled.size());
    } else if (!linear_bgra && !format.compressed) {
        const size_t texel_bytes = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8 ? 1u : 4u;
        const size_t packed_size = static_cast<size_t>(desc.width) * desc.height * texel_bytes;
        if (packed_size > draw.texture_byte_count) return 0;
        if (isSwizzledTextureFormat(desc.format_byte) && !desc.linear) {
            unswizzled.resize(packed_size);
            if (!recomp_d3d_texture_unswizzle(
                    static_cast<const uint8_t *>(draw.texture_bytes),
                    unswizzled.data(), desc.width, desc.height, static_cast<uint32_t>(texel_bytes))) {
                return 0;
            }
            upload_bytes = unswizzled.data();
            upload_size = static_cast<uint32_t>(unswizzled.size());
        }
    }

    GLuint gpu_tex = 0;
    glGenTextures(1, &gpu_tex);
    glBindTexture(GL_TEXTURE_2D, gpu_tex);
    uint32_t uploaded_levels = levels;
    if (format.compressed) {
        const uint32_t block_bytes = format.internal_format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? 8u : 16u;
        uint32_t cur_w = desc.width, cur_h = desc.height, cur_offset = 0u;
        for (uint32_t lvl = 0; lvl < levels; ++lvl) {
            const uint32_t level_size = ((cur_w + 3u) / 4u) * block_bytes * ((cur_h + 3u) / 4u);
            if (level_size > upload_size - cur_offset) {
                uploaded_levels = lvl; // keep the levels the guest bytes cover
                break;
            }
            glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(lvl), format.internal_format,
                static_cast<GLsizei>(cur_w), static_cast<GLsizei>(cur_h), 0,
                static_cast<GLsizei>(level_size), upload_bytes + cur_offset);
            cur_offset += level_size;
            cur_w = cur_w > 1u ? cur_w / 2u : 1u;
            cur_h = cur_h > 1u ? cur_h / 2u : 1u;
        }
        if (uploaded_levels == 0u) {
            glDeleteTextures(1, &gpu_tex);
            return 0;
        }
    } else if (linear_bgra) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(desc.width),
            static_cast<GLsizei>(desc.height), 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        uploadLinearBgra(gpu_tex, desc, upload_bytes);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format.internal_format),
            static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height), 0,
            format.format, format.type, upload_bytes);
        if (format.internal_format == GL_R8) {
            // Like SDL's A8_UNORM: sample as (0, 0, 0, a).
            const GLint swizzle[4] = {GL_ZERO, GL_ZERO, GL_ZERO, GL_RED};
            glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
        }
    }
    // Otherwise the default 1000 levels leave a mipmapped sampler incomplete.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(uploaded_levels - 1u));

    const uint32_t slot = presenter->next_texture_slot;
    if (presenter->textures[slot].used) {
        releaseTexture(presenter, slot);
    }
    TextureEntry &entry = presenter->textures[slot];
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

// The shared generators emit Vulkan GLSL 450. GL 4.3 takes the same source once
// descriptor sets are dropped: TransformUniforms -> UBO 0 in both stages,
// ProgramConstants -> UBO 1, guest_texture/alpha_mask -> texture units 0/1.
std::string glslForGl(std::string source)
{
    const std::string version = "#version 450";
    if (source.compare(0, version.size(), version) == 0) {
        source.replace(0, version.size(), "#version 430 core");
    }
    const std::string set = "layout(set = ";
    for (size_t at = source.find(set); at != std::string::npos; at = source.find(set, at)) {
        const size_t comma = source.find(", ", at);
        if (comma == std::string::npos) break;
        source.erase(at + 7u, comma + 2u - (at + 7u));
    }
    return source;
}

GLuint compileShader(GLenum stage, const std::string &source)
{
    const GLuint shader = glCreateShader(stage);
    const char *text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        std::fprintf(stderr, "[presenter] GLSL %s shader failed: %s\n",
            stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint linkProgram(GLuint vs, GLuint fs)
{
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDetachShader(program, vs);
    glDetachShader(program, fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetProgramInfoLog(program, sizeof log, nullptr, log);
        std::fprintf(stderr, "[presenter] GLSL link failed: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// Attribute locations match the generated shader's layout(location = N) inputs.
GLuint createVertexArray(RecompD3dPresenter *presenter, const RecompD3dVertexLayout &layout)
{
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, presenter->draw_index_buffer);
    const auto attribute = [](GLuint location, GLint size, GLenum type, GLboolean normalized, uint32_t offset) {
        glEnableVertexAttribArray(location);
        glVertexAttribFormat(location, size, type, normalized, offset);
        glVertexAttribBinding(location, 0);
    };
    attribute(0, layout.pretransformed ? 4 : 3, GL_FLOAT, GL_FALSE, layout.position_offset);
    if (layout.blend_weight_count != 0u) {
        attribute(1, static_cast<GLint>(std::min(layout.blend_weight_count, 3u)), GL_FLOAT, GL_FALSE, 12u);
    }
    if (layout.normal_offset != RECOMP_D3D_FVF_ABSENT) {
        attribute(2, 3, GL_FLOAT, GL_FALSE, layout.normal_offset);
    }
    if (layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT) {
        attribute(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, layout.diffuse_offset);
    }
    if (layout.texcoord_offset != RECOMP_D3D_FVF_ABSENT) {
        const uint32_t tc_count = (layout.texcoord_count == 4u) ? 4u : ((layout.texcoord_count == 2u) ? 2u : 1u);
        for (uint32_t i = 0u; i < tc_count; ++i) {
            attribute(4u + i, 2, GL_FLOAT, GL_FALSE, layout.texcoord_offset + i * 8u);
        }
    }
    glBindVertexArray(0);
    return vao;
}

// Keyed like SDL's shader pairs. Failures are cached too, so a shader that
// cannot build is not retried on every draw.
ShaderProgram getOrCreateShaderProgram(RecompD3dPresenter *presenter, const RecompD3dVertexLayout &layout,
                                       uint32_t fvf, uint32_t program_count, const uint32_t (*program_tokens)[4])
{
    uint64_t shader_key = fvf ^ (static_cast<uint64_t>(program_count) << 32);
    if (program_count && program_tokens) {
        shader_key ^= programHash(program_tokens, program_count);
    }
    const auto it = presenter->shader_cache.find(shader_key);
    if (it != presenter->shader_cache.end()) {
        return it->second;
    }

    ShaderProgram result{};
    // The D3D11 presenter accepts vertex programs only with this FVF.
    if (!program_count || fvf == 0x112u) {
        const std::string vs_source = recomp_glsl_vertex_shader(layout, fvf, program_count, program_tokens);
        // Like the D3D11 presenter, skip a draw whose program does not translate.
        if (!vs_source.empty()) {
            const GLuint vs = compileShader(GL_VERTEX_SHADER, glslForGl(vs_source));
            const GLuint fs = compileShader(GL_FRAGMENT_SHADER,
                glslForGl(recomp_glsl_fragment_shader(layout, fvf, program_count)));
            if (vs && fs) result.program = linkProgram(vs, fs);
            glDeleteShader(vs);
            glDeleteShader(fs);
        }
        if (result.program) {
            result.vao = createVertexArray(presenter, layout);
        } else {
            std::fprintf(stderr, "[presenter] failed to build GLSL program fvf=0x%08X\n", fvf);
        }
    }
    presenter->shader_cache.emplace(shader_key, result);
    return result;
}

DrawState drawState(const RecompD3dPresenterDrawCommand &draw)
{
    DrawState s{};
    s.cull_mode = draw.cull_mode;

    s.depth_test_enable = draw.depth.depth_test_enable;
    s.depth_write_enable = draw.depth.depth_write_enable;
    s.depth_func = (s.depth_test_enable || s.depth_write_enable)
        ? draw.depth.depth_func : RECOMP_D3D_COMPARE_LESS_EQUAL;

    s.stencil_enable = draw.depth.stencil_enable;
    if (s.stencil_enable) {
        s.stencil_ref = static_cast<uint8_t>(draw.depth.stencil_ref);
        s.stencil_read_mask = static_cast<uint8_t>(draw.depth.stencil_read_mask);
        s.stencil_write_mask = static_cast<uint8_t>(draw.depth.stencil_write_mask);
        s.stencil_func = draw.depth.stencil_func;
        s.stencil_fail = draw.depth.stencil_fail;
        s.stencil_zfail = draw.depth.stencil_zfail;
        s.stencil_pass = draw.depth.stencil_pass;
    }

    s.blend_enable = draw.blend.blend_enable;
    s.color_write_mask = draw.blend.color_write_mask;
    if (s.blend_enable) {
        s.src_factor = draw.blend.src_factor;
        s.dst_factor = draw.blend.dst_factor;
        s.blend_op = draw.blend.op;
        s.constant_color = draw.blend.constant_color;
    }
    return s;
}

// Sets every field, so no state from an earlier draw or clear carries over.
void applyDrawState(const DrawState &s)
{
    if (s.cull_mode == RECOMP_D3D_CULL_NONE) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        // UPPER_LEFT clip control negates the facing area, which cancels its
        // Y flip: GL winding equals the guest's, as in the SDL and D3D11
        // presenters, so culling clockwise keeps counter-clockwise faces.
        glFrontFace(s.cull_mode == RECOMP_D3D_CULL_CLOCKWISE ? GL_CCW : GL_CW);
    }

    if (s.depth_test_enable) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    glDepthFunc(hostCompareOp(s.depth_func));
    glDepthMask(s.depth_write_enable ? GL_TRUE : GL_FALSE);

    if (s.stencil_enable) glEnable(GL_STENCIL_TEST);
    else glDisable(GL_STENCIL_TEST);
    glStencilFunc(hostCompareOp(s.stencil_func), s.stencil_ref, s.stencil_read_mask);
    glStencilOp(hostStencilOp(s.stencil_fail), hostStencilOp(s.stencil_zfail), hostStencilOp(s.stencil_pass));
    glStencilMask(s.stencil_write_mask);

    if (s.blend_enable) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);
    glBlendFuncSeparate(hostBlendFactor(s.src_factor, false), hostBlendFactor(s.dst_factor, false),
                        hostBlendFactor(s.src_factor, true), hostBlendFactor(s.dst_factor, true));
    glBlendEquation(hostBlendOp(s.blend_op));
    const uint32_t color = s.constant_color;
    glBlendColor(((color >> 16u) & 255u) / 255.0f, ((color >> 8u) & 255u) / 255.0f,
                 (color & 255u) / 255.0f, ((color >> 24u) & 255u) / 255.0f);
    glColorMask((s.color_write_mask & 1u) ? GL_TRUE : GL_FALSE, (s.color_write_mask & 2u) ? GL_TRUE : GL_FALSE,
                (s.color_write_mask & 4u) ? GL_TRUE : GL_FALSE, (s.color_write_mask & 8u) ? GL_TRUE : GL_FALSE);
}

void flushSegment(RecompD3dPresenter *presenter)
{
    DrawSegment &seg = presenter->segment;
    if (seg.draws.empty()) return;
    const auto flush_start = std::chrono::steady_clock::now();
    struct FlushTimer {
        RecompD3dPresenter *p;
        std::chrono::steady_clock::time_point start;
        ~FlushTimer() {
            p->second_flush_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
        }
    } timer{presenter, flush_start};
    if (seg.draws.empty()) return;

    // Every draw binds a whole TransformUniforms block; the tail keeps the
    // last one inside the upload.
    seg.uniform_data.resize(seg.uniform_data.size() + kTransformUniformFloats, 0.0f);

    const uint32_t seg_v_bytes = static_cast<uint32_t>(seg.vertex_data.size());
    const uint32_t seg_i_bytes = static_cast<uint32_t>(seg.index_data.size());
    const uint32_t seg_u_bytes = static_cast<uint32_t>(seg.uniform_data.size() * sizeof(float));

    if (ensureDynamicBuffers(presenter, seg_v_bytes, seg_i_bytes, seg_u_bytes) &&
        bindTarget(presenter, seg.target_color, seg.target_depth, seg.target_w, seg.target_h)) {
        const uint32_t seg_v_base = streamUpload(presenter->draw_vertex_buffer, presenter->draw_vertex_capacity,
            presenter->draw_vertex_used, seg.vertex_data.data(), seg_v_bytes);
        const uint32_t seg_i_base = streamUpload(presenter->draw_index_buffer, presenter->draw_index_capacity,
            presenter->draw_index_used, seg.index_data.data(), seg_i_bytes);
        const uint32_t seg_u_base = streamUpload(presenter->draw_uniform_buffer, presenter->draw_uniform_capacity,
            presenter->draw_uniform_used, seg.uniform_data.data(), seg_u_bytes);

        // Wider than any GL name or offset, so the first draw sets everything.
        constexpr uint32_t kUnset = UINT32_MAX;
        uint32_t last_program = kUnset, last_vao = kUnset;
        uint32_t last_uniform_offset = kUnset, last_program_offset = kUnset;
        uint32_t last_t0 = kUnset, last_s0 = kUnset, last_t1 = kUnset, last_s1 = kUnset;
        bool have_state = false;
        DrawState last_state{};
        uint32_t last_stride = kUnset;

        for (const QueuedDraw &q : seg.draws) {
            if (q.program != last_program) {
                glUseProgram(q.program);
                last_program = q.program;
            }
            // The binding lives in the vertex array. Vertex offsets are whole
            // strides from the segment base, so draws reach them by base vertex.
            if (q.vao != last_vao || q.vertex_stride != last_stride) {
                glBindVertexArray(q.vao);
                glBindVertexBuffer(0, presenter->draw_vertex_buffer,
                    static_cast<GLintptr>(seg_v_base), static_cast<GLsizei>(q.vertex_stride));
                last_vao = q.vao;
                last_stride = q.vertex_stride;
            }

            if (q.uniform_offset != last_uniform_offset) {
                glBindBufferRange(GL_UNIFORM_BUFFER, 0, presenter->draw_uniform_buffer,
                    static_cast<GLintptr>(seg_u_base + q.uniform_offset * sizeof(float)),
                    kTransformUniformFloats * sizeof(float));
                last_uniform_offset = q.uniform_offset;
            }
            if (q.program_offset != kNoProgramConstants && q.program_offset != last_program_offset) {
                glBindBufferRange(GL_UNIFORM_BUFFER, 1, presenter->draw_uniform_buffer,
                    static_cast<GLintptr>(seg_u_base + q.program_offset * sizeof(float)),
                    kProgramConstantFloats * sizeof(float));
                last_program_offset = q.program_offset;
            }

            const GLuint t0 = q.texture ? q.texture : presenter->dummy_texture;
            const GLuint t1 = q.mask_texture ? q.mask_texture : presenter->dummy_texture;
            if (t0 != last_t0) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, t0);
                last_t0 = t0;
            }
            if (t1 != last_t1) {
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, t1);
                last_t1 = t1;
            }
            if (q.sampler0 != last_s0) {
                glBindSampler(0, q.sampler0);
                last_s0 = q.sampler0;
            }
            if (q.sampler1 != last_s1) {
                glBindSampler(1, q.sampler1);
                last_s1 = q.sampler1;
            }

            if (!have_state || !(q.state == last_state)) {
                applyDrawState(q.state);
                last_state = q.state;
                have_state = true;
            }

            glDrawElementsBaseVertex(q.mode, static_cast<GLsizei>(q.index_count), GL_UNSIGNED_SHORT,
                reinterpret_cast<const void *>(static_cast<uintptr_t>(seg_i_base + q.index_offset)),
                static_cast<GLint>(q.vertex_offset / q.vertex_stride));
        }
    }

    seg.draws.clear();
    seg.vertex_data.clear();
    seg.index_data.clear();
    seg.uniform_data.clear();
}

bool createGlResources(RecompD3dPresenter *p)
{
    const ClipControlFn clip_control = hostClipControl();
    if (!clip_control) {
        std::fprintf(stderr, "[presenter] glClipControl is unavailable\n");
        return false;
    }
    clip_control(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    if (std::getenv("RECOMP_GL_DEBUG")) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(reportGlMessage, nullptr);
    }
    glDisable(GL_DITHER);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const uint32_t main_w = mainWidth(p);
    const uint32_t main_h = mainHeight(p);
    p->backbuffer_color = createTargetTexture(main_w, main_h);
    p->backbuffer_copy = createTargetTexture(main_w, main_h);
    p->frontbuffer_copy = createTargetTexture(main_w, main_h);
    glGenRenderbuffers(1, &p->backbuffer_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, p->backbuffer_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
        static_cast<GLsizei>(main_w), static_cast<GLsizei>(main_h));

    // Dummy texture (1x1 white)
    const uint32_t white_pixel = 0xffffffffu;
    glGenTextures(1, &p->dummy_texture);
    glBindTexture(GL_TEXTURE_2D, p->dummy_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white_pixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    // Scales the backbuffer into the window target. Targets hold the top row
    // first, and glReadPixels hands rows to the framebuffer in that order.
    static const char kPresentVs[] =
        "#version 430 core\n"
        "out vec2 uv;\n"
        "void main() {\n"
        "    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2.0;\n"
        "    uv = vec2(corner.x, 1.0 - corner.y);\n"
        "    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char kPresentFs[] =
        "#version 430 core\n"
        "in vec2 uv;\n"
        "layout(binding = 0) uniform sampler2D source;\n"
        "out vec4 color;\n"
        "void main() { color = vec4(texture(source, uv).rgb, 1.0); }\n";
    const GLuint present_vs = compileShader(GL_VERTEX_SHADER, kPresentVs);
    const GLuint present_fs = compileShader(GL_FRAGMENT_SHADER, kPresentFs);
    if (present_vs && present_fs) p->present_program = linkProgram(present_vs, present_fs);
    if (present_vs) glDeleteShader(present_vs);
    if (present_fs) glDeleteShader(present_fs);
    if (!p->present_program) return false;
    glGenVertexArrays(1, &p->present_vao);

    p->filter_sampler = createSampler(GL_LINEAR_MIPMAP_NEAREST, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
    p->program_mask_sampler = createSampler(GL_LINEAR_MIPMAP_NEAREST, GL_REPEAT, GL_REPEAT);

    p->draw_vertex_buffer = createStreamBuffer(p->draw_vertex_capacity);
    p->draw_index_buffer = createStreamBuffer(p->draw_index_capacity);
    p->draw_uniform_buffer = createStreamBuffer(p->draw_uniform_capacity);

    GLint align = static_cast<GLint>(kStreamAlignment);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    if (align <= 0 || static_cast<uint32_t>(align) > kStreamAlignment || kStreamAlignment % align != 0) {
        std::fprintf(stderr, "[presenter] unsupported uniform buffer alignment %d\n", align);
        return false;
    }
    p->uniform_align_floats = std::max(1u, static_cast<uint32_t>(align) / 4u);

    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        std::fprintf(stderr, "[presenter] GL setup failed 0x%x\n", static_cast<unsigned>(error));
        return false;
    }
    std::fprintf(stderr, "[presenter] %s | %s\n",
        reinterpret_cast<const char *>(glGetString(GL_RENDERER)),
        reinterpret_cast<const char *>(glGetString(GL_VERSION)));
    return true;
}

// glDelete* ignores zero names, so this also cleans up a partial create.
void releaseResources(RecompD3dPresenter *p)
{
    for (auto &entry : p->textures) {
        if (entry.used) glDeleteTextures(1, &entry.texture);
    }
    p->textures.clear();
    p->texture_index.clear();
    for (auto &rt : p->render_targets) glDeleteTextures(1, &rt.texture);
    p->render_targets.clear();
    for (auto &dt : p->depth_targets) glDeleteRenderbuffers(1, &dt.renderbuffer);
    p->depth_targets.clear();
    for (auto &pair : p->framebuffers) glDeleteFramebuffers(1, &pair.second);
    p->framebuffers.clear();
    for (auto &pair : p->shader_cache) {
        glDeleteProgram(pair.second.program);
        glDeleteVertexArrays(1, &pair.second.vao);
    }
    p->shader_cache.clear();
    glDeleteProgram(p->present_program);
    glDeleteVertexArrays(1, &p->present_vao);

    glDeleteBuffers(1, &p->draw_vertex_buffer);
    glDeleteBuffers(1, &p->draw_index_buffer);
    glDeleteBuffers(1, &p->draw_uniform_buffer);
    for (auto &row : p->address_samplers) glDeleteSamplers(6, row);
    glDeleteSamplers(1, &p->filter_sampler);
    glDeleteSamplers(1, &p->program_mask_sampler);
    glDeleteTextures(1, &p->dummy_texture);
    glDeleteTextures(1, &p->backbuffer_color);
    glDeleteRenderbuffers(1, &p->backbuffer_depth);
    glDeleteTextures(1, &p->backbuffer_copy);
    glDeleteTextures(1, &p->frontbuffer_copy);
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

    float scale = 1.0f;
    if (const char *scale_env = std::getenv("RECOMP_D3D_SCALE")) {
        const float val = static_cast<float>(std::atof(scale_env));
        if (std::isfinite(val) && val >= 1.0f) {
            scale = std::clamp(val, 1.0f, 8.0f);
        }
    }
    const char *widescreen_env = std::getenv("RECOMP_D3D_WIDESCREEN");
    const bool widescreen = (widescreen_env == nullptr || std::strcmp(widescreen_env, "0") != 0);

    auto *p = new (std::nothrow) RecompD3dPresenter();
    if (!p) {
        return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    }
    p->config = *config;
    p->scale = scale;
    p->widescreen = widescreen;
    p->owner_thread = std::this_thread::get_id();
    const char *performance = std::getenv("RECOMP_PERF_COUNTER");
    p->performance_counter = performance != nullptr && std::strcmp(performance, "1") == 0;

    if (!hostCreate(p)) {
        hostDestroy(p);
        delete p;
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    if (!createGlResources(p)) {
        releaseResources(p);
        hostDestroy(p);
        delete p;
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }

    std::fprintf(stderr, "[presenter] initialized scale=%.2f resolution=%ux%u widescreen=%d\n",
        scale, mainWidth(p), mainHeight(p), widescreen ? 1 : 0);

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
    // Events are pumped once per present.
    if (presenter->close_requested) {
        return RECOMP_D3D_PRESENTER_CLOSED;
    }
    if (command == nullptr) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    struct SubmitTimer {
        RecompD3dPresenter *p;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~SubmitTimer() {
            if (p) p->second_submit_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
        }
    } timer{presenter->performance_counter ? presenter : nullptr};

    switch (command->type) {
    case RECOMP_D3D_PRESENTER_COMMAND_CLEAR: {
        const auto &clear = command->data.clear;
        if (clear.clear_depth && (!std::isfinite(clear.z) || clear.z < 0.0f || clear.z > 1.0f)) {
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }

        flushSegment(presenter);

        GLuint target_color = presenter->backbuffer_color;
        uint32_t color_w = mainWidth(presenter);
        uint32_t color_h = mainHeight(presenter);

        if (clear.target.offscreen) {
            color_w = clear.target.color.width;
            color_h = clear.target.color.height;
            RenderTargetEntry *rt = ensureRenderTarget(presenter, clear.target.color);
            if (!rt) return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
            target_color = rt->texture;
        }
        const GLuint target_depth = lookupDepthTarget(presenter, clear.target, color_w, color_h);
        const bool use_depth = (target_depth != 0) && (clear.clear_depth || clear.clear_stencil);
        if (!clear.clear_color && !use_depth) {
            return RECOMP_D3D_PRESENTER_OK;
        }
        if (!bindTarget(presenter, target_color, target_depth, color_w, color_h)) {
            return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }

        // Clears obey the write masks; the next flush restores its draw state.
        if (clear.clear_color) {
            constexpr float b2f = 1.0f / 255.0f;
            const GLfloat color[4] = {
                static_cast<float>((clear.color >> 16u) & 0xffu) * b2f,
                static_cast<float>((clear.color >> 8u) & 0xffu) * b2f,
                static_cast<float>(clear.color & 0xffu) * b2f,
                static_cast<float>((clear.color >> 24u) & 0xffu) * b2f,
            };
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearBufferfv(GL_COLOR, 0, color);
        }
        if (use_depth) {
            const GLint stencil = static_cast<GLint>(clear.stencil & 0xffu);
            glDepthMask(GL_TRUE);
            glStencilMask(0xffu);
            if (clear.clear_depth && clear.clear_stencil) {
                glClearBufferfi(GL_DEPTH_STENCIL, 0, clear.z, stencil);
            } else if (clear.clear_depth) {
                glClearBufferfv(GL_DEPTH, 0, &clear.z);
            } else {
                glClearBufferiv(GL_STENCIL, 0, &stencil);
            }
        }
        return RECOMP_D3D_PRESENTER_OK;
    }

    case RECOMP_D3D_PRESENTER_COMMAND_PRESENT: {
        const auto &present = command->data.present;
        if (present.effective_flags != 5u || present.swap_counter == 0u) {
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }

        flushSegment(presenter);

        pumpEvents(presenter);
        if (presenter->close_requested) {
            return RECOMP_D3D_PRESENTER_CLOSED;
        }

        const GLsizei main_w = static_cast<GLsizei>(mainWidth(presenter));
        const GLsizei main_h = static_cast<GLsizei>(mainHeight(presenter));
        int swap_w = 0, swap_h = 0;
        const GLuint window_target = hostWindowTarget(presenter, &swap_w, &swap_h);
        if (swap_w > 0 && swap_h > 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, window_target);
            glViewport(0, 0, swap_w, swap_h);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            const float src_aspect = static_cast<float>(main_w) / main_h;
            const float dst_aspect = static_cast<float>(swap_w) / swap_h;
            int dst_x = 0, dst_y = 0, dst_w = swap_w, dst_h = swap_h;
            if (dst_aspect > src_aspect) {
                dst_w = static_cast<int>(swap_h * src_aspect + 0.5f);
                dst_x = (swap_w - dst_w) / 2;
            } else {
                dst_h = static_cast<int>(swap_w / src_aspect + 0.5f);
                dst_y = (swap_h - dst_h) / 2;
            }
            // The next segment sets its full draw state again.
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glViewport(dst_x, dst_y, dst_w, dst_h);
            glUseProgram(presenter->present_program);
            glBindVertexArray(presenter->present_vao);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, presenter->backbuffer_color);
            glBindSampler(0, presenter->filter_sampler);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        glCopyImageSubData(presenter->backbuffer_color, GL_TEXTURE_2D, 0, 0, 0, 0,
                           presenter->frontbuffer_copy, GL_TEXTURE_2D, 0, 0, 0, 0, main_w, main_h, 1);

        const auto swap_start = std::chrono::steady_clock::now();
        hostSwap(presenter);
        presenter->second_swap_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - swap_start).count();
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
        GLuint target_color = presenter->backbuffer_color;
        uint32_t target_color_data = 0;
        uint32_t color_w = mainWidth(presenter);
        uint32_t color_h = mainHeight(presenter);

        if (draw.target.offscreen) {
            target_color_data = draw.target.color.data;
            color_w = draw.target.color.width;
            color_h = draw.target.color.height;
            RenderTargetEntry *rt = ensureRenderTarget(presenter, draw.target.color);
            if (rt && rt->texture) {
                target_color = rt->texture;
            }
        }

        const GLuint target_depth = lookupDepthTarget(presenter, draw.target, color_w, color_h);
        const float target_w = static_cast<float>(color_w);
        const float target_h = static_cast<float>(color_h);

        // Target switch or sampling current render target requires flushing active segment
        if (presenter->segment.target_color != target_color ||
            presenter->segment.target_depth != target_depth ||
            (draw.has_texture && (draw.texture_is_backbuffer || (draw.texture.data == presenter->segment.target_color_data && presenter->segment.target_color_data != 0u)))) {
            flushSegment(presenter);
        }

        presenter->segment.target_color = target_color;
        presenter->segment.target_depth = target_depth;
        presenter->segment.target_color_data = target_color_data;
        presenter->segment.target_w = color_w;
        presenter->segment.target_h = color_h;

        // Prepare textures
        const GLuint texture = draw.has_texture ? prepareTexture(presenter, draw) : 0;
        GLuint mask_texture = 0;

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
        if (v_offset % draw.vertex_stride != 0u) { // flushSegment draws by base vertex
            vdata.resize(v_offset + draw.vertex_stride - v_offset % draw.vertex_stride, 0);
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

        // A draw whose program failed is skipped, as before.
        const ShaderProgram shaders = getOrCreateShaderProgram(presenter, layout, draw.fvf, draw.program_count,
                                                               draw.program_count ? draw.program : nullptr);
        if (!shaders.program) {
            vdata.resize(v_offset);
            idata.resize(i_offset);
            return RECOMP_D3D_PRESENTER_OK;
        }

        const GLuint s0 = (draw.four_tap_filter || draw.has_alpha_mask || draw.program_count)
            ? presenter->filter_sampler
            : lookupDrawSampler(presenter, draw.address_u, draw.address_v);
        // Like D3D11, the reflection environment map uses the draw's own sampler. GLSL binds
        // one sampler per texture, so slot 1 carries it.
        const GLuint s1 = draw.program_alpha_mask ? presenter->program_mask_sampler
            : (draw.has_reflection && !draw.has_alpha_mask) ? s0 : presenter->filter_sampler;

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

        constants[64] = (texture != 0) ? 1.0f : 0.0f;
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

        // Only lit draws store the directional block; vertex programs store
        // their constants in the second uniform block.
        const uint32_t vs_uniform_size = (draw.directional.enabled && !draw.program_count)
            ? kTransformUniformFloats : kHeaderUniformFloats;

        auto &udata = presenter->segment.uniform_data;
        // Uniform block bindings must start on the device's offset alignment.
        const uint32_t align = presenter->uniform_align_floats;
        const auto alignUniforms = [&udata, align] {
            udata.resize((udata.size() + align - 1u) / align * align, 0.0f);
        };
        uint32_t uniform_offset = 0;
        bool reused_uniforms = false;
        if (!presenter->segment.draws.empty()) {
            const auto &prev = presenter->segment.draws.back();
            if (prev.vs_uniform_size == vs_uniform_size &&
                prev.uniform_offset + vs_uniform_size <= udata.size() &&
                std::memcmp(constants, &udata[prev.uniform_offset], vs_uniform_size * sizeof(float)) == 0) {
                uniform_offset = prev.uniform_offset;
                reused_uniforms = true;
            }
        }

        if (!reused_uniforms) {
            alignUniforms();
            uniform_offset = static_cast<uint32_t>(udata.size());
            udata.insert(udata.end(), constants, constants + vs_uniform_size);
        }
        uint32_t program_offset = kNoProgramConstants;
        if (draw.program_count) {
            alignUniforms();
            program_offset = static_cast<uint32_t>(udata.size());
            const float *program_constants = &draw.program_constants[0][0];
            udata.insert(udata.end(), program_constants, program_constants + kProgramConstantFloats);
        }

        QueuedDraw q{};
        q.program = shaders.program;
        q.vao = shaders.vao;
        q.mode = draw.primitive_type == RECOMP_D3D_PT_TRIANGLESTRIP ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
        q.state = drawState(draw);
        q.vertex_offset = v_offset;
        q.vertex_stride = draw.vertex_stride;
        q.index_offset = i_offset;
        q.index_count = queued_index_count;
        q.texture = texture;
        q.mask_texture = mask_texture;
        q.sampler0 = s0;
        q.sampler1 = s1;
        q.uniform_offset = uniform_offset;
        q.vs_uniform_size = vs_uniform_size;
        q.program_offset = program_offset;

        presenter->segment.draws.push_back(q);
        ++presenter->draw_count;
        return RECOMP_D3D_PRESENTER_OK;
    }

    // Like the SDL GPU presenter, the gamma ramp is accepted and not applied.
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
        if (presenter->textures[i].used && released(presenter->textures[i].data)) {
            releaseTexture(presenter, i);
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
    flushSegment(p);
    releaseResources(p);
    hostDestroy(p);

    delete p;
    active_presenter = nullptr;
    *presenter = nullptr;
    return RECOMP_D3D_PRESENTER_OK;
}

// Takes effect at the next present, on the thread that owns the GL context.
void recomp_d3d_presenter_set_immediate_present(bool enabled)
{
    immediate_present_setting = enabled;
}

bool recomp_d3d_presenter_split_enabled(void)
{
    return false;
}

void recomp_d3d_presenter_report_draw_textures(void)
{
}

} // extern "C"
