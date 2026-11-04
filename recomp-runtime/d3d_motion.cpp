#include "d3d_motion.h"

#include "d3d_matrix.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

template <typename T>
void release(T *&object)
{
    if (object != nullptr) object->Release();
    object = nullptr;
}

struct MotionConstants {
    float inverse_camera[16];   // this frame's unjittered clip space to world
    float previous_camera[16];  // world to the previous frame's clip space
    float jitter[2];            // clip-space jitter baked into this frame's depth
    float size[2];              // render target pixels
};

struct ObjectHistory {
    float world[4][16];
    float view_projection[16];
    uint32_t matrices;
};

void replaceAll(std::string &text, const std::string &from, const std::string &to)
{
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
}

} // namespace

struct D3dMotion {
    uint32_t width = 0u, height = 0u;
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    ID3D11PixelShader *debug_ps = nullptr;
    ID3D11BlendState *debug_blend = nullptr;
    ID3D11Buffer *constants = nullptr;
    ID3D11RenderTargetView *target = nullptr; // R16G16_FLOAT, render pixels
    ID3D11ShaderResourceView *view = nullptr;
    /* Object vectors: scene draws write rg = pixels moved, b = depth, a = 1
       here; the reprojection keeps them where that surface is still visible. */
    ID3D11RenderTargetView *object_target = nullptr; // R16G16B16A16_FLOAT
    ID3D11ShaderResourceView *object_view = nullptr;
    /* Keyed by mesh. A mesh drawn several times (foliage) matches each copy to
       last frame's nearest by world position: draw order changes as the game
       sorts by distance, so order alone pairs the wrong copies. */
    std::unordered_map<uint64_t, std::vector<ObjectHistory>> previous, current;
    size_t recorded = 0u;
};

void d3d_motion_destroy(D3dMotion *motion)
{
    if (motion == nullptr) return;
    release(motion->vs);
    release(motion->ps);
    release(motion->debug_ps);
    release(motion->debug_blend);
    release(motion->constants);
    release(motion->target);
    release(motion->view);
    release(motion->object_target);
    release(motion->object_view);
    delete motion;
}

D3dMotion *d3d_motion_create(ID3D11Device *device, ID3D11DeviceContext *context,
    uint32_t width, uint32_t height, bool multisampled, bool objects)
{
    std::string source = multisampled
        ? "Texture2DMS<float> scene_depth : register(t0);\n"
          "float loadDepth(int2 p) { return scene_depth.Load(p, 0); }\n"
        : "Texture2D<float> scene_depth : register(t0);\n"
          "float loadDepth(int2 p) { return scene_depth.Load(int3(p, 0)); }\n";
    source +=
        "Texture2D<float2> motion : register(t1);\n"
        "Texture2D<float4> object_motion : register(t2);\n"
        "cbuffer Motion : register(b0) {\n"
        "    row_major float4x4 inverse_camera;\n"
        "    row_major float4x4 previous_camera;\n"
        "    float2 jitter;\n"
        "    float2 size;\n"
        "};\n"
        "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
        "    float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
        "float2 reproject(float4 p : SV_Position) : SV_Target {\n"
        "    float depth = loadDepth(int2(p.xy));\n"
        // An object's own vector, if it is still the surface the depth buffer kept.
        "    float4 object = object_motion.Load(int3(p.xy, 0));\n"
        "    if (object.a > 0.5 && abs(object.b - depth) < 2e-5) return object.rg;\n"
        "    float2 ndc = p.xy / size * float2(2, -2) + float2(-1, 1) - jitter;\n"
        "    float4 world = mul(float4(ndc, depth, 1), inverse_camera);\n"
        "    float4 previous = mul(world / world.w, previous_camera);\n"
        "    if (!(previous.w > 0)) return float2(0, 0);\n"
        "    float2 delta = (previous.xy / previous.w - ndc) * float2(0.5, -0.5) * size;\n"
        "    return all(isfinite(delta)) ? delta : float2(0, 0); }\n"
        // Hue is the direction, brightness the length; 16 pixels saturates.
        // size is the output's here; the vectors may be at the smaller DLSS input size.
        "float4 debug(float4 p : SV_Position) : SV_Target {\n"
        "    uint w, h; motion.GetDimensions(w, h);\n"
        "    float2 v = motion.Load(int3(p.xy * float2(w, h) / size, 0));\n"
        "    float length = saturate(sqrt(dot(v, v)) / 16);\n"
        "    float hue = atan2(v.y, v.x) / 6.2831853 + 0.5;\n"
        "    float3 rgb = saturate(abs(frac(hue + float3(0, 2, 1) / 3) * 6 - 3) - 1);\n"
        "    return float4(lerp(float3(0.2, 0.2, 0.2), rgb, length), 0.8); }\n";
    auto *motion = new D3dMotion;
    motion->width = width;
    motion->height = height;
    ID3DBlob *code[3]{}, *errors = nullptr;
    const char *entries[3] = {"vs", "reproject", "debug"};
    const char *profiles[3] = {"vs_4_1", "ps_4_1", "ps_4_1"};
    HRESULT result = S_OK;
    for (int i = 0; i < 3 && SUCCEEDED(result); ++i) {
        result = D3DCompile(source.data(), source.size(), "motion.hlsl", nullptr, nullptr,
            entries[i], profiles[i], 0u, 0u, &code[i], &errors);
        if (FAILED(result) && errors != nullptr) {
            std::fprintf(stderr, "recomp motion: %s: %.*s\n", entries[i],
                static_cast<int>(errors->GetBufferSize()),
                static_cast<const char *>(errors->GetBufferPointer()));
        }
        release(errors);
    }
    if (SUCCEEDED(result)) result = device->CreateVertexShader(
        code[0]->GetBufferPointer(), code[0]->GetBufferSize(), nullptr, &motion->vs);
    if (SUCCEEDED(result)) result = device->CreatePixelShader(
        code[1]->GetBufferPointer(), code[1]->GetBufferSize(), nullptr, &motion->ps);
    if (SUCCEEDED(result)) result = device->CreatePixelShader(
        code[2]->GetBufferPointer(), code[2]->GetBufferSize(), nullptr, &motion->debug_ps);
    for (auto *blob : code) release(blob);
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(MotionConstants);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(result)) result = device->CreateBuffer(&buffer, nullptr, &motion->constants);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1u;
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    desc.SampleDesc.Count = 1u;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *texture = nullptr;
    if (SUCCEEDED(result)) result = device->CreateTexture2D(&desc, nullptr, &texture);
    if (SUCCEEDED(result)) result = device->CreateRenderTargetView(texture, nullptr, &motion->target);
    if (SUCCEEDED(result)) result = device->CreateShaderResourceView(texture, nullptr, &motion->view);
    release(texture);
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (SUCCEEDED(result)) result = device->CreateBlendState(&blend, &motion->debug_blend);
    if (FAILED(result)) {
        std::fprintf(stderr, "recomp motion: unavailable hr=0x%08lX\n", static_cast<unsigned long>(result));
        d3d_motion_destroy(motion);
        return nullptr;
    }
    if (objects) {
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ID3D11Texture2D *texture = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) &&
            SUCCEEDED(device->CreateRenderTargetView(texture, nullptr, &motion->object_target)) &&
            SUCCEEDED(device->CreateShaderResourceView(texture, nullptr, &motion->object_view))) {
            const float zero[4] = {};
            context->ClearRenderTargetView(motion->object_target, zero);
        } else {
            release(motion->object_target);
            release(motion->object_view);
        }
        release(texture);
    }
    return motion;
}

ID3D11RenderTargetView *d3d_motion_target(D3dMotion *motion)
{
    return motion->target;
}

ID3D11RenderTargetView *d3d_motion_object_target(D3dMotion *motion)
{
    return motion != nullptr ? motion->object_target : nullptr;
}

void d3d_motion_reproject(D3dMotion *motion, ID3D11DeviceContext *context,
    ID3D11ShaderResourceView *depth, const float *camera, const float *previous,
    const float jitter[2])
{
    context->ClearState();
    MotionConstants constants{};
    if (camera == nullptr || !matrixInverse(camera, constants.inverse_camera)) {
        const float zero[4] = {};
        context->ClearRenderTargetView(motion->target, zero);
        return;
    }
    std::memcpy(constants.previous_camera, previous, sizeof constants.previous_camera);
    constants.jitter[0] = jitter[0];
    constants.jitter[1] = jitter[1];
    constants.size[0] = static_cast<float>(motion->width);
    constants.size[1] = static_cast<float>(motion->height);
    context->UpdateSubresource(motion->constants, 0u, nullptr, &constants, 0u, 0u);
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, constants.size[0], constants.size[1], 0.0f, 1.0f};
    context->RSSetViewports(1u, &viewport);
    context->OMSetRenderTargets(1u, &motion->target, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(motion->vs, nullptr, 0u);
    context->PSSetShader(motion->ps, nullptr, 0u);
    context->PSSetConstantBuffers(0u, 1u, &motion->constants);
    ID3D11ShaderResourceView *views[3] = {depth, nullptr, motion->object_view};
    context->PSSetShaderResources(0u, 3u, views);
    context->Draw(3u, 0u);
    context->ClearState();
}

void d3d_motion_draw_debug(D3dMotion *motion, ID3D11DeviceContext *context,
    ID3D11RenderTargetView *output, uint32_t width, uint32_t height)
{
    context->ClearState();
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width),
        static_cast<float>(height), 0.0f, 1.0f};
    MotionConstants constants{};
    constants.size[0] = viewport.Width;
    constants.size[1] = viewport.Height;
    context->UpdateSubresource(motion->constants, 0u, nullptr, &constants, 0u, 0u);
    context->PSSetConstantBuffers(0u, 1u, &motion->constants);
    context->RSSetViewports(1u, &viewport);
    context->OMSetRenderTargets(1u, &output, nullptr);
    context->OMSetBlendState(motion->debug_blend, nullptr, 0xffffffffu);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(motion->vs, nullptr, 0u);
    context->PSSetShader(motion->debug_ps, nullptr, 0u);
    ID3D11ShaderResourceView *views[2] = {nullptr, motion->view};
    context->PSSetShaderResources(0u, 2u, views);
    context->Draw(3u, 0u);
    context->ClearState();
}

bool d3d_motion_object(D3dMotion *motion, const RecompD3dPresenterDrawCommand &draw,
    float *previous_wvp)
{
    if (motion->object_view == nullptr || draw.program_count || !draw.has_transform ||
        draw.vertex_address == 0u || draw.blend_weight_count >= 4u) {
        return false;
    }
    const uint64_t mesh = (static_cast<uint64_t>(draw.vertex_address) << 32) ^
        (static_cast<uint64_t>(draw.index_count) * 0x9E3779B1u) ^
        (static_cast<uint64_t>(draw.vertex_count) << 12) ^ draw.fvf;
    const uint32_t matrices = draw.blend_weight_count + 1u;
    const ObjectHistory *match = nullptr;
    const auto previous = motion->previous.find(mesh);
    if (previous != motion->previous.end()) {
        float nearest = INFINITY;
        for (const auto &candidate : previous->second) {
            if (candidate.matrices != matrices) continue;
            float distance = 0.0f;
            for (int axis = 12; axis < 15; ++axis) {
                const float delta = candidate.world[0][axis] - draw.world[0][axis];
                distance += delta * delta;
            }
            if (distance < nearest) { nearest = distance; match = &candidate; }
        }
    }
    if (match != nullptr) {
        Float4x4 view_projection;
        std::memcpy(&view_projection, match->view_projection, sizeof view_projection);
        for (uint32_t i = 0u; i < matrices; ++i) {
            Float4x4 world;
            std::memcpy(&world, match->world[i], sizeof world);
            const Float4x4 product = world * view_projection;
            std::memcpy(previous_wvp + 16u * i, &product, sizeof product);
        }
    }
    if (motion->recorded < 16384u) {
        ++motion->recorded;
        ObjectHistory history{};
        std::memcpy(history.world, draw.world, sizeof(float) * 16u * matrices);
        std::memcpy(history.view_projection, draw.view_projection, sizeof history.view_projection);
        history.matrices = matrices;
        motion->current[mesh].push_back(history);
    }
    return match != nullptr;
}

void d3d_motion_end_frame(D3dMotion *motion, ID3D11DeviceContext *context)
{
    if (motion->object_target == nullptr) return;
    std::swap(motion->previous, motion->current);
    motion->current.clear();
    motion->recorded = 0u;
    const float zero[4] = {};
    context->ClearRenderTargetView(motion->object_target, zero);
}

/* Fixed-function vertex shaders also place the vertex with last frame's
   matrices; the pixel shader writes the difference in pixels and its depth to
   SV_Target1 when motion_flags.x is set. Vertex programs keep camera vectors.
   Other variants (sun shadows, per-pixel lighting, vertex programs) edit the
   same source first, so every anchor is checked before anything changes.
   Edits run from the end of the source back, so earlier offsets stay valid. */
void d3d_motion_shader_source(std::string &source)
{
    constexpr size_t npos = std::string::npos;
    const std::string fields = "struct VSOut {", output = "VSOut output;";
    const std::string lit = "output.lit_specular=0;\n";
    const std::string jitter = "    output.position.xy += taa_jitter.xy * output.position.w;\n";
    const size_t fields_at = source.find(fields);
    const size_t output_at = source.find(output);
    const size_t pixel = source.find("float4 ps_main(");
    const size_t signature_end = pixel == npos ? npos : source.find(") : SV_TARGET", pixel);
    const size_t body = signature_end == npos ? npos : source.find('{', signature_end);
    if (fields_at == npos || output_at == npos || body == npos ||
        fields_at > output_at || output_at > pixel) return;

    source.insert(body + 1, R"(
    motion_output = 0;
    if (motion_flags.x > 0.5f && input.motion_current.w > 0 && input.motion_previous.w > 0) {
        float2 now = input.motion_current.xy / input.motion_current.w;
        float2 before = input.motion_previous.xy / input.motion_previous.w;
        motion_output = float4((before - now) * float2(0.5f, -0.5f) * motion_flags.zw, input.position.z, 1);
    }
)");
    source.insert(signature_end, ", out float4 motion_output : SV_Target1");
    // Fixed function: repeat the position code with last frame's matrices.
    const size_t jitter_at = source.find(jitter, output_at);
    const size_t lit_at = source.find(lit, output_at);
    if (jitter_at != npos && jitter_at < pixel && lit_at != npos && lit_at < jitter_at) {
        const size_t position_at = lit_at + lit.size();
        std::string previous = source.substr(position_at, jitter_at - position_at);
        replaceAll(previous, "wvp[", "previous_wvp[");
        replaceAll(previous, "output.position", "motion_previous");
        source.insert(jitter_at, "    output.motion_current = output.position;\n"
            "    float4 motion_previous;\n" + previous +
            "    output.motion_previous = motion_previous;\n");
    }
    source.insert(output_at + output.size(), " output.motion_current = 0; output.motion_previous = 0;");
    // TEXCOORD9/10: the slots no other variant (pixel lighting, sun, G-buffer) uses.
    source.insert(fields_at + fields.size(),
        "\n    float4 motion_current : TEXCOORD9;\n    float4 motion_previous : TEXCOORD10;\n");
}

float d3d_motion_halton(uint32_t index, uint32_t base)
{
    float fraction = 1.0f, result = 0.0f;
    while (index > 0u) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}
