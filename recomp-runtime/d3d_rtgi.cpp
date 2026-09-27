#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <vector>

#include "d3d_rtgi.h"
#include "d3d_draw_model.h"
#include "rtgi_trace_shader.h"
#include "rtgi_temporal_shader.h"
#include "rtgi_spatial_shader.h"

using Microsoft::WRL::ComPtr;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4X4;

struct RtgiError { HRESULT result; const char *operation; };

static void check(HRESULT result, const char *operation)
{
    if (FAILED(result)) throw RtgiError{result, operation};
}

struct RtgiTexture {
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D11Texture2D> imported;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};

struct RtgiSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList4> commands;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12Resource> vertices, indices, instances, constants;
    ComPtr<ID3D12Resource> bottom, top, scratch;
    UINT64 completion = 0;
};

struct D3dRtgi {
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D12Device5> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> ready12, done12;
    ComPtr<ID3D11Fence> ready11, done11;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> trace, temporal, spatial;
    ComPtr<ID3D11VertexShader> fullscreen;
    ComPtr<ID3D11PixelShader> reduce[2], composite[2];
    ComPtr<ID3D11SamplerState> linear;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11Buffer> constants11;
    RtgiTexture depth, color, gi, raw, filtered, history[2], position[2], normal[2];
    RtgiSlot slots[3];
    std::vector<XMFLOAT3> positions;
    std::vector<uint32_t> indices;
    XMFLOAT4X4 camera{}, previousCamera{};
    size_t cameraTriangles = 0;
    UINT width = 0, height = 0, halfWidth = 0, halfHeight = 0;
    UINT64 frame = 0, readyValue = 0, doneValue = 0;
    float strength = 1;
    bool failed = false, historyValid = false;
};

static void fail(D3dRtgi &r, const char *operation, HRESULT result = E_FAIL)
{
    if (!r.failed)
        std::fprintf(stderr, "recomp rtgi: %s (0x%08lX)\n", operation,
            static_cast<unsigned long>(result));
    r.failed = true;
}

static void waitFence(D3dRtgi &r, ID3D12Fence *fence, UINT64 value)
{
    if (!fence || !value) return;
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        check(r.device12->GetDeviceRemovedReason(), "D3D12 device removed");
        check(r.device11->GetDeviceRemovedReason(), "D3D11 device removed");
        const UINT64 completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) throw RtgiError{DXGI_ERROR_DEVICE_REMOVED, "fence device removed"};
        if (completed >= value) return;
        if (GetTickCount64() - start >= 10000)
            throw RtgiError{HRESULT_FROM_WIN32(WAIT_TIMEOUT), "GPU fence timed out after 10 s"};
        Sleep(1);
    }
}

static void ensureBuffer(D3dRtgi &r, ComPtr<ID3D12Resource> &resource,
    UINT64 size, bool upload, bool acceleration = false)
{
    if (resource && resource->GetDesc().Width >= size) return;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    const UINT64 oldSize = resource ? resource->GetDesc().Width : 0;
    desc.Width = (std::max(size, oldSize * 2) + 65535) & ~UINT64(65535);
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = upload ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto state = upload ? D3D12_RESOURCE_STATE_GENERIC_READ : acceleration
        ? D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE : D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12Resource> replacement;
    check(r.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        state, nullptr, IID_PPV_ARGS(&replacement)), "create frame buffer");
    resource = replacement;
}

static void upload(D3dRtgi &r, ComPtr<ID3D12Resource> &resource, const void *data, size_t size)
{
    ensureBuffer(r, resource, size, true);
    void *mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    check(resource->Map(0, &noRead, &mapped), "map upload buffer");
    std::memcpy(mapped, data, size);
    const D3D12_RANGE written{0, size};
    resource->Unmap(0, &written);
}

static void texture(D3dRtgi &r, RtgiTexture &t, DXGI_FORMAT format, bool shared, bool renderTarget)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = r.halfWidth;
    desc.Height = r.halfHeight;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.Flags = renderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET :
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (shared) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
        D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    check(r.device12->CreateCommittedResource(&heap,
        shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t.resource)), "create GI texture");
    if (!shared) return;
    HANDLE handle = nullptr;
    check(r.device12->CreateSharedHandle(t.resource.Get(), nullptr, GENERIC_ALL,
        nullptr, &handle), "share texture");
    const HRESULT opened = r.device11->OpenSharedResource1(handle, IID_PPV_ARGS(&t.imported));
    CloseHandle(handle);
    check(opened, "open shared texture in D3D11");
    check(r.device11->CreateShaderResourceView(t.imported.Get(), nullptr, &t.srv), "create shared SRV");
    if (renderTarget)
        check(r.device11->CreateRenderTargetView(t.imported.Get(), nullptr, &t.rtv), "create shared RTV");
}

static const char fullscreenSource[] = R"(
Texture2D<float4> colorImage : register(t0);
#if MULTISAMPLED
Texture2DMS<float> depthImage : register(t1);
float readDepth(int2 p) { return depthImage.Load(p, 0); }
#else
Texture2D<float> depthImage : register(t1);
float readDepth(int2 p) { return depthImage.Load(int3(p, 0)); }
#endif
Texture2D<float4> giImage : register(t2);
SamplerState linearClamp : register(s0);
cbuffer Frame : register(b0) { float2 fullSize; float strength; float split; };
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vs(uint id : SV_VertexID) {
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
struct Reduced { float depth : SV_Target0; float4 color : SV_Target1; };
Reduced reducePS(Vertex v) {
    int2 p = min(int2(v.uv * fullSize), int2(fullSize) - 1);
    Reduced output;
    output.depth = readDepth(p);
    output.color = colorImage.Load(int3(p, 0));
    return output;
}
float4 compositePS(Vertex v, uint sampleIndex : SV_SampleIndex) : SV_Target {
    int2 p = int2(v.position.xy);
#if MULTISAMPLED
    float depth = depthImage.Load(p, sampleIndex);
#else
    float depth = readDepth(p);
#endif
    if (depth >= 1 || v.uv.x < split) return 0;
    // The 8-bit target cannot exceed white: fade the gain as the brightest
    // channel nears 1 so bright walls keep their texture and hue.
    float3 base = colorImage.SampleLevel(linearClamp, v.uv, 0).rgb;
    float headroom = saturate(1 - max(base.r, max(base.g, base.b)));
    return float4(strength * headroom * giImage.SampleLevel(linearClamp, v.uv, 0).rgb, 0);
}
)";

static ComPtr<ID3DBlob> compile11(const char *entry, const char *target, bool msaa)
{
    const D3D_SHADER_MACRO macros[] = {{"MULTISAMPLED", msaa ? "1" : "0"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> blob, errors;
    check(D3DCompile(fullscreenSource, sizeof fullscreenSource - 1, nullptr, macros,
        nullptr, entry, target, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS,
        0, &blob, &errors), "compile D3D11 RTGI shader");
    return blob;
}

static void initialize(D3dRtgi &r, ID3D11Device *device)
{
    if (!device || !r.width || !r.height) throw RtgiError{E_INVALIDARG, "invalid device or dimensions"};
    check(device->QueryInterface(IID_PPV_ARGS(&r.device11)), "ID3D11Device5 shared fences unavailable");
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    check(context.As(&r.context11), "ID3D11DeviceContext4 shared fences unavailable");
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    check(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)), "query DXGI device");
    check(dxgiDevice->GetAdapter(&adapter), "get D3D11 adapter");
    DXGI_ADAPTER_DESC adapterDesc{};
    check(adapter->GetDesc(&adapterDesc), "describe D3D11 adapter");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(&r.device12)), "D3D12 device unavailable on D3D11 adapter");
    const LUID luid = r.device12->GetAdapterLuid();
    if (luid.LowPart != adapterDesc.AdapterLuid.LowPart || luid.HighPart != adapterDesc.AdapterLuid.HighPart)
        throw RtgiError{E_FAIL, "D3D11/D3D12 adapter LUID mismatch"};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    check(r.device12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
        &options, sizeof options), "query DXR 1.1 support");
    if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
        throw RtgiError{E_NOINTERFACE, "DXR 1.1 unavailable"};
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    check(r.device12->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof model),
        "query Shader Model 6.5 support");
    if (model.HighestShaderModel < D3D_SHADER_MODEL_6_5)
        throw RtgiError{E_NOINTERFACE, "Shader Model 6.5 unavailable"};

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(r.device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&r.queue)), "create D3D12 queue");
    auto sharedFence = [&](ComPtr<ID3D12Fence> &f12, ComPtr<ID3D11Fence> &f11) {
        check(r.device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f12)), "create shared fence");
        HANDLE handle = nullptr;
        check(r.device12->CreateSharedHandle(f12.Get(), nullptr, GENERIC_ALL, nullptr, &handle), "share fence");
        const HRESULT opened = r.device11->OpenSharedFence(handle, IID_PPV_ARGS(&f11));
        CloseHandle(handle);
        check(opened, "open shared fence in D3D11");
    };
    sharedFence(r.ready12, r.ready11);
    sharedFence(r.done12, r.done11);

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 11, 1, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 0, 0, 11};
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[3].Constants = {1, 0, 1};
    rootDesc.NumParameters = 4;
    rootDesc.pParameters = parameters;
    ComPtr<ID3DBlob> blob, errors;
    check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
        "serialize RTGI root signature");
    check(r.device12->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&r.root)), "create RTGI root signature");
    auto pipeline = [&](const void *bytes, size_t size, ComPtr<ID3D12PipelineState> &out) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = r.root.Get();
        desc.CS = {bytes, size};
        check(r.device12->CreateComputePipelineState(&desc, IID_PPV_ARGS(&out)), "create RTGI compute pipeline");
    };
    pipeline(kRtgiTraceShader, sizeof kRtgiTraceShader, r.trace);
    pipeline(kRtgiTemporalShader, sizeof kRtgiTemporalShader, r.temporal);
    pipeline(kRtgiSpatialShader, sizeof kRtgiSpatialShader, r.spatial);
    for (auto &slot : r.slots) {
        check(r.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&slot.allocator)), "create frame allocator");
        check(r.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(),
            nullptr, IID_PPV_ARGS(&slot.commands)), "create frame command list");
        check(slot.commands->Close(), "close initial command list");
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 4 * 16;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(r.device12->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&slot.descriptors)), "create frame descriptors");
    }
    texture(r, r.depth, DXGI_FORMAT_R32_FLOAT, true, true);
    texture(r, r.color, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
    texture(r, r.gi, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false);
    texture(r, r.raw, DXGI_FORMAT_R16G16B16A16_FLOAT, false, false);
    texture(r, r.filtered, DXGI_FORMAT_R16G16B16A16_FLOAT, false, false);
    for (unsigned i = 0; i < 2; ++i) {
        texture(r, r.history[i], DXGI_FORMAT_R16G16B16A16_FLOAT, false, false);
        texture(r, r.position[i], DXGI_FORMAT_R32G32B32A32_FLOAT, false, false);
        texture(r, r.normal[i], DXGI_FORMAT_R16G16B16A16_FLOAT, false, false);
    }
    auto vs = compile11("vs", "vs_5_0", false);
    check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
        &r.fullscreen), "create fullscreen vertex shader");
    for (unsigned i = 0; i < 2; ++i) {
        auto reduce = compile11("reducePS", "ps_5_0", i != 0);
        auto composite = compile11("compositePS", "ps_5_0", i != 0);
        check(device->CreatePixelShader(reduce->GetBufferPointer(), reduce->GetBufferSize(), nullptr,
            &r.reduce[i]), "create reduction shader");
        check(device->CreatePixelShader(composite->GetBufferPointer(), composite->GetBufferSize(), nullptr,
            &r.composite[i]), "create composite shader");
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    check(device->CreateSamplerState(&sampler, &r.linear), "create bilinear sampler");
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = 16;
    constants.Usage = D3D11_USAGE_IMMUTABLE;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    // RECOMP_D3D_RTGI_SPLIT=1 leaves the left half unlit for side-by-side comparison.
    const char *split = std::getenv("RECOMP_D3D_RTGI_SPLIT");
    const float values[] = {float(r.width), float(r.height), r.strength, split && *split == '1' ? 0.5f : 0.0f};
    const D3D11_SUBRESOURCE_DATA data{values, 0, 0};
    check(device->CreateBuffer(&constants, &data, &r.constants11), "create composite constants");
    D3D11_BLEND_DESC blend{};
    auto &target = blend.RenderTarget[0];
    target.BlendEnable = true;
    target.SrcBlend = D3D11_BLEND_DEST_COLOR;
    target.DestBlend = D3D11_BLEND_ONE;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ZERO;
    target.DestBlendAlpha = D3D11_BLEND_ONE;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
        D3D11_COLOR_WRITE_ENABLE_BLUE;
    check(device->CreateBlendState(&blend, &r.blend), "create GI blend state");
}

D3dRtgi *d3d_rtgi_create(ID3D11Device *device, uint32_t width, uint32_t height)
{
    try {
        auto r = std::make_unique<D3dRtgi>();
        r->width = width;
        r->height = height;
        r->halfWidth = width / 2 + width % 2;
        r->halfHeight = height / 2 + height % 2;
        if (const char *text = std::getenv("RECOMP_D3D_RTGI_STRENGTH")) {
            char *end = nullptr;
            const float value = std::strtof(text, &end);
            if (end != text && *end == '\0' && std::isfinite(value)) r->strength = std::clamp(value, 0.0f, 4.0f);
        }
        initialize(*r, device);
        return r.release();
    } catch (const RtgiError &error) {
        std::fprintf(stderr, "recomp rtgi: %s (0x%08lX)\n", error.operation,
            static_cast<unsigned long>(error.result));
    } catch (const std::exception &error) {
        std::fprintf(stderr, "recomp rtgi: %s\n", error.what());
    }
    return nullptr;
}

void d3d_rtgi_destroy(D3dRtgi *r)
{
    if (!r) return;
    try {
        r->context11->Flush();
        waitFence(*r, r->done12.Get(), r->doneValue);
        waitFence(*r, r->ready12.Get(), r->readyValue);
    } catch (const RtgiError &error) {
        // The GPU may still use these resources; leaking them beats a use-after-free.
        fail(*r, error.operation, error.result);
        return;
    }
    delete r;
}

bool d3d_rtgi_add_draw(D3dRtgi *r, const RecompD3dPresenterDrawCommand &draw)
{
    // Vertex-program draws (water) move vertices in their program; WORLD alone would misplace them.
    if (!r || r->failed || !draw.has_transform || draw.target.offscreen || draw.program_count ||
        !draw.depth.depth_write_enable || !draw.triangle_count || !draw.vertex_count ||
        !draw.vertex_bytes || !draw.index_bytes || draw.primitive_type < 5 || draw.primitive_type > 7) return false;
    RecompD3dVertexLayout layout{};
    if (!recomp_d3d_fvf_layout(draw.fvf, &layout) || layout.pretransformed ||
        draw.blend_weight_count > 3 || draw.blend_weight_count > layout.blend_weight_count ||
        draw.vertex_stride < 12 + 4 * layout.blend_weight_count ||
        uint64_t(r->positions.size()) + draw.vertex_count > UINT32_MAX) return false;
    for (float value : draw.view_projection) if (!std::isfinite(value)) return false;
    const size_t base = r->positions.size(), firstIndex = r->indices.size();
    try {
        // ponytail: CPU skinning is sufficient here; move to compute above roughly 1M vertices/frame.
        for (uint32_t v = 0; v < draw.vertex_count; ++v) {
            const auto *bytes = static_cast<const unsigned char *>(draw.vertex_bytes) + size_t(v) * draw.vertex_stride;
            float p[3], weights[3]{};
            std::memcpy(p, bytes, sizeof p);
            std::memcpy(weights, bytes + 12, draw.blend_weight_count * sizeof(float));
            XMFLOAT3 transformed{};
            float remainder = 1;
            for (uint32_t i = 0; i <= draw.blend_weight_count; ++i) {
                const float weight = i == draw.blend_weight_count ? remainder : weights[i];
                if (i < draw.blend_weight_count) remainder -= weight;
                const float *m = draw.world[i];
                transformed.x += weight * (p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12]);
                transformed.y += weight * (p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13]);
                transformed.z += weight * (p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14]);
            }
            if (!std::isfinite(transformed.x) || !std::isfinite(transformed.y) || !std::isfinite(transformed.z)) {
                r->positions.resize(base);
                return false;
            }
            r->positions.push_back(transformed);
        }
        const uint32_t triangles = std::min(draw.triangle_count,
            recomp_d3d_draw_triangle_count(draw.primitive_type, draw.index_count));
        auto indexAt = [&](uint32_t i) {
            uint16_t index;
            std::memcpy(&index, static_cast<const unsigned char *>(draw.index_bytes) + size_t(i) * 2, 2);
            return uint32_t(index);
        };
        for (uint32_t t = 0; t < triangles; ++t) {
            uint32_t a, b, c;
            if (draw.primitive_type == 5) { a = indexAt(t * 3); b = indexAt(t * 3 + 1); c = indexAt(t * 3 + 2); }
            else if (draw.primitive_type == 6) {
                a = indexAt(t); b = indexAt(t + 1); c = indexAt(t + 2);
                if (t & 1) std::swap(a, b);
            } else { a = indexAt(0); b = indexAt(t + 1); c = indexAt(t + 2); }
            if (a >= draw.vertex_count || b >= draw.vertex_count || c >= draw.vertex_count ||
                a == b || b == c || a == c) continue;
            const auto pa = DirectX::XMLoadFloat3(&r->positions[base + a]);
            const auto pb = DirectX::XMLoadFloat3(&r->positions[base + b]);
            const auto pc = DirectX::XMLoadFloat3(&r->positions[base + c]);
            const auto area = DirectX::XMVector3Cross(DirectX::XMVectorSubtract(pb, pa), DirectX::XMVectorSubtract(pc, pa));
            if (!(DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(area)) > 0)) continue;
            if (r->indices.size() > UINT32_MAX - 3) throw RtgiError{E_OUTOFMEMORY, "too many scene indices"};
            r->indices.insert(r->indices.end(), {uint32_t(base + a), uint32_t(base + b), uint32_t(base + c)});
        }
        const size_t contributed = (r->indices.size() - firstIndex) / 3;
        if (!contributed) r->positions.resize(base);
        if (contributed > r->cameraTriangles) {
            r->cameraTriangles = contributed;
            std::memcpy(&r->camera, draw.view_projection, sizeof r->camera);
        }
        return contributed != 0;
    } catch (const RtgiError &error) { fail(*r, error.operation, error.result); }
    catch (const std::exception &error) { fail(*r, error.what()); }
    r->positions.resize(base);
    r->indices.resize(firstIndex);
    return false;
}

static void transition(ID3D12GraphicsCommandList *commands, ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    commands->ResourceBarrier(1, &barrier);
}

static void uavBarrier(ID3D12GraphicsCommandList *commands, ID3D12Resource *resource)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    commands->ResourceBarrier(1, &barrier);
}

static void buildScene(D3dRtgi &r, RtgiSlot &slot)
{
    upload(r, slot.vertices, r.positions.data(), r.positions.size() * sizeof(XMFLOAT3));
    upload(r, slot.indices, r.indices.data(), r.indices.size() * sizeof(uint32_t));
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.VertexCount = UINT(r.positions.size());
    geometry.Triangles.VertexBuffer = {slot.vertices->GetGPUVirtualAddress(), sizeof(XMFLOAT3)};
    geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geometry.Triangles.IndexCount = UINT(r.indices.size());
    geometry.Triangles.IndexBuffer = slot.indices->GetGPUVirtualAddress();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};
    bottom.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottom.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    bottom.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bottom.NumDescs = 1;
    bottom.pGeometryDescs = &geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomSize{}, topSize{};
    r.device12->GetRaytracingAccelerationStructurePrebuildInfo(&bottom, &bottomSize);
    if (!bottomSize.ResultDataMaxSizeInBytes) throw RtgiError{E_FAIL, "invalid BLAS prebuild size"};
    ensureBuffer(r, slot.bottom, bottomSize.ResultDataMaxSizeInBytes, false, true);
    D3D12_RAYTRACING_INSTANCE_DESC instance{};
    instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1;
    instance.InstanceMask = 0xff;
    instance.AccelerationStructure = slot.bottom->GetGPUVirtualAddress();
    upload(r, slot.instances, &instance, sizeof instance);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
    top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.NumDescs = 1;
    top.InstanceDescs = slot.instances->GetGPUVirtualAddress();
    r.device12->GetRaytracingAccelerationStructurePrebuildInfo(&top, &topSize);
    if (!topSize.ResultDataMaxSizeInBytes) throw RtgiError{E_FAIL, "invalid TLAS prebuild size"};
    ensureBuffer(r, slot.top, topSize.ResultDataMaxSizeInBytes, false, true);
    ensureBuffer(r, slot.scratch, std::max(bottomSize.ScratchDataSizeInBytes, topSize.ScratchDataSizeInBytes), false);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = bottom;
    build.DestAccelerationStructureData = slot.bottom->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = slot.scratch->GetGPUVirtualAddress();
    auto *commands = slot.commands.Get();
    // Reused result and scratch buffers were read/written by an earlier frame.
    uavBarrier(commands, nullptr);
    commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    uavBarrier(commands, slot.bottom.Get());
    uavBarrier(commands, slot.scratch.Get());
    build.Inputs = top;
    build.DestAccelerationStructureData = slot.top->GetGPUVirtualAddress();
    commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    uavBarrier(commands, slot.top.Get());
}

static void traceFrame(D3dRtgi &r, RtgiSlot &slot, const XMFLOAT4X4 &inverse)
{
    struct Constants {
        XMFLOAT4X4 inverse, camera, previous;
        UINT width, height, fullWidth, fullHeight, frame, history;
        float padding[2];
        XMFLOAT4X4 rayInverse;
        DirectX::XMFLOAT4 eye;
    };
    static_assert(sizeof(Constants) == 304, "HLSL frame layout");
    Constants constants{inverse, r.camera, r.previousCamera, r.halfWidth, r.halfHeight,
        r.width, r.height, UINT(r.frame), UINT(r.historyValid), {0, 0}};
    auto eye = DirectX::XMVector4Transform(DirectX::XMVectorSet(0, 0, 1, 0), DirectX::XMLoadFloat4x4(&inverse));
    const bool perspective = std::abs(DirectX::XMVectorGetW(eye)) > 1e-8f;
    eye = perspective ? DirectX::XMVectorScale(eye, 1 / DirectX::XMVectorGetW(eye)) : DirectX::XMVectorZero();
    DirectX::XMStoreFloat4(&constants.eye, eye);
    // Unproject relative to the eye: subtracting two near-plane world points
    // loses the ray direction when the camera is far from the world origin.
    const auto relative = DirectX::XMMatrixTranslationFromVector(eye) * DirectX::XMLoadFloat4x4(&r.camera);
    DirectX::XMStoreFloat4x4(&constants.rayInverse, DirectX::XMMatrixInverse(nullptr, relative));
    upload(r, slot.constants, &constants, sizeof constants);
    const unsigned current = unsigned(r.frame & 1), previous = current ^ 1;
    ID3D12Resource *reads[] = {r.depth.resource.Get(), r.color.resource.Get(),
        r.history[previous].resource.Get(), r.position[previous].resource.Get(),
        r.raw.resource.Get(), r.history[current].resource.Get(),
        r.position[current].resource.Get(), r.normal[current].resource.Get(),
        r.normal[previous].resource.Get()};
    ID3D12Resource *writes[] = {r.raw.resource.Get(), r.history[current].resource.Get(),
        r.gi.resource.Get(), r.position[current].resource.Get(), r.normal[current].resource.Get()};
    auto descriptor = slot.descriptors->GetCPUDescriptorHandleForHeapStart();
    const UINT stride = r.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12Resource *filterInputs[] = {writes[1], r.filtered.resource.Get(), writes[0], r.filtered.resource.Get()};
    ID3D12Resource *filterOutputs[] = {r.filtered.resource.Get(), writes[0], r.filtered.resource.Get(), writes[2]};
    for (unsigned pass = 0; pass < 4; ++pass) {
        for (unsigned i = 0; i < 9; ++i) {
            auto *resource = i == 5 ? filterInputs[pass] : reads[i];
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = resource->GetDesc().Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            r.device12->CreateShaderResourceView(resource, &view, descriptor);
            descriptor.ptr += stride;
        }
        ID3D12Resource *geometry[] = {slot.vertices.Get(), slot.indices.Get()};
        const UINT counts[] = {UINT(r.positions.size()), UINT(r.indices.size())};
        const UINT sizes[] = {sizeof(XMFLOAT3), sizeof(uint32_t)};
        for (unsigned i = 0; i < 2; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Buffer.NumElements = counts[i];
            view.Buffer.StructureByteStride = sizes[i];
            r.device12->CreateShaderResourceView(geometry[i], &view, descriptor);
            descriptor.ptr += stride;
        }
        for (unsigned i = 0; i < 5; ++i) {
            auto *resource = i == 2 ? filterOutputs[pass] : writes[i];
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = resource->GetDesc().Format;
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            r.device12->CreateUnorderedAccessView(resource, nullptr, &view, descriptor);
            descriptor.ptr += stride;
        }
    }
    auto *commands = slot.commands.Get();
    constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
    constexpr auto srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    for (unsigned i = 0; i < 4; ++i) transition(commands, reads[i], common, srv);
    transition(commands, reads[8], common, srv);
    for (unsigned i : {0u, 1u, 3u, 4u}) transition(commands, writes[i], common, uav);
    commands->SetComputeRootSignature(r.root.Get());
    commands->SetDescriptorHeaps(1, slot.descriptors.GetAddressOf());
    commands->SetComputeRootShaderResourceView(0, slot.top->GetGPUVirtualAddress());
    commands->SetComputeRootConstantBufferView(1, slot.constants->GetGPUVirtualAddress());
    commands->SetComputeRootDescriptorTable(2, slot.descriptors->GetGPUDescriptorHandleForHeapStart());
    const UINT x = (r.halfWidth + 7) / 8, y = (r.halfHeight + 7) / 8;
    commands->SetPipelineState(r.trace.Get());
    commands->Dispatch(x, y, 1);
    transition(commands, writes[0], uav, srv);
    transition(commands, writes[3], uav, srv);
    transition(commands, writes[4], uav, srv);
    commands->SetPipelineState(r.temporal.Get());
    commands->Dispatch(x, y, 1);
    transition(commands, writes[1], uav, srv);
    commands->SetPipelineState(r.spatial.Get());
    auto table = slot.descriptors->GetGPUDescriptorHandleForHeapStart();
    for (unsigned pass = 0; pass < 4; ++pass) {
        // raw is no longer needed by temporal; reuse it as a filter scratch image.
        transition(commands, filterOutputs[pass], pass == 0 || pass == 3 ? common : srv, uav);
        commands->SetComputeRootDescriptorTable(2, table);
        commands->SetComputeRoot32BitConstant(3, 1u << pass, 0);
        commands->Dispatch(x, y, 1);
        transition(commands, filterOutputs[pass], uav, pass == 3 ? common : srv);
        table.ptr += 16 * stride;
    }
    for (auto *resource : reads) transition(commands, resource, srv, common);
    transition(commands, r.filtered.resource.Get(), srv, common);
}

static void fullscreen(D3dRtgi &r, ID3D11DeviceContext *context, ID3D11PixelShader *shader,
    UINT width, UINT height, ID3D11ShaderResourceView *const *inputs, UINT inputCount,
    ID3D11RenderTargetView *const *targets, UINT targetCount, ID3D11BlendState *blend = nullptr)
{
    context->ClearState();
    const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(r.fullscreen.Get(), nullptr, 0);
    context->PSSetShader(shader, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, r.constants11.GetAddressOf());
    context->PSSetSamplers(0, 1, r.linear.GetAddressOf());
    context->PSSetShaderResources(0, inputCount, inputs);
    context->OMSetRenderTargets(targetCount, targets, nullptr);
    context->OMSetBlendState(blend, nullptr, UINT_MAX);
    context->Draw(3, 0);
    context->ClearState();
}

void d3d_rtgi_apply(D3dRtgi *r, ID3D11DeviceContext *context, ID3D11RenderTargetView *target,
    ID3D11ShaderResourceView *color, ID3D11ShaderResourceView *depth, uint32_t depthSamples)
{
    bool applied = false;
    if (r && !r->failed && !r->indices.empty() && context && target && color && depth) {
        try {
            if (context != r->context11.Get()) throw RtgiError{E_INVALIDARG, "unexpected D3D11 context"};
            check(r->device12->GetDeviceRemovedReason(), "D3D12 device removed");
            check(r->device11->GetDeviceRemovedReason(), "D3D11 device removed");
            DirectX::XMVECTOR determinant;
            const auto inverseMatrix = DirectX::XMMatrixInverse(&determinant, DirectX::XMLoadFloat4x4(&r->camera));
            XMFLOAT4X4 inverse;
            DirectX::XMStoreFloat4x4(&inverse, inverseMatrix);
            bool invertible = DirectX::XMVectorGetX(determinant) != 0;
            for (const auto &row : inverse.m) for (float value : row) invertible &= std::isfinite(value);
            if (invertible) {
                auto &slot = r->slots[r->frame % 3];
                waitFence(*r, r->done12.Get(), slot.completion);
                check(slot.allocator->Reset(), "reset frame allocator");
                check(slot.commands->Reset(slot.allocator.Get(), nullptr), "reset frame commands");
                buildScene(*r, slot);
                traceFrame(*r, slot, inverse);
                check(slot.commands->Close(), "close frame commands");

                ID3D11ShaderResourceView *inputs[] = {color, depth, r->gi.srv.Get()};
                ID3D11RenderTargetView *targets[] = {r->depth.rtv.Get(), r->color.rtv.Get()};
                fullscreen(*r, context, r->reduce[depthSamples > 1].Get(), r->halfWidth, r->halfHeight,
                    inputs, 2, targets, 2);
                const UINT64 inputValue = r->frame * 2 + 1, doneValue = r->frame + 1;
                check(r->context11->Signal(r->ready11.Get(), inputValue), "signal D3D11 input ready");
                r->readyValue = inputValue;
                context->Flush();
                check(r->queue->Wait(r->ready12.Get(), inputValue), "D3D12 wait for input");
                ID3D12CommandList *lists[] = {slot.commands.Get()};
                r->queue->ExecuteCommandLists(1, lists);
                check(r->queue->Signal(r->done12.Get(), doneValue), "signal GI ready");
                r->doneValue = slot.completion = doneValue;
                check(r->context11->Wait(r->done11.Get(), doneValue), "D3D11 wait for GI");
                // The reduced color stands in for the target, which cannot be read while bound.
                inputs[0] = r->color.srv.Get();
                fullscreen(*r, context, r->composite[depthSamples > 1].Get(), r->width, r->height,
                    inputs, 3, &target, 1, r->blend.Get());
                check(r->context11->Signal(r->ready11.Get(), inputValue + 1), "signal GI consumed");
                r->readyValue = inputValue + 1;
                context->Flush();
                check(r->device12->GetDeviceRemovedReason(), "D3D12 frame execution");
                check(r->device11->GetDeviceRemovedReason(), "D3D11 frame execution");
                r->previousCamera = r->camera;
                r->historyValid = true;
                ++r->frame;
                applied = true;
            }
        } catch (const RtgiError &error) { fail(*r, error.operation, error.result); }
        catch (const std::exception &error) { fail(*r, error.what()); }
    }
    if (r) {
        r->positions.clear();
        r->indices.clear();
        r->cameraTriangles = 0;
        if (!applied) r->historyValid = false;
    }
    if (context) context->ClearState();
}
