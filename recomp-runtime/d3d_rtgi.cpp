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
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include "d3d_rtgi.h"
#include "d3d_draw_model.h"
#include "d3d_tuning.h"
#include "rtgi_trace_shader.h"
#include "rtgi_temporal_shader.h"
#include "rtgi_spatial_shader.h"

using Microsoft::WRL::ComPtr;
using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4X4;

constexpr UINT alphaSize = 256, alphaSlices = 256, frameDescriptors = 26;
constexpr uint32_t opaqueMaterial = UINT32_MAX;

struct RtgiError { HRESULT result; const char *operation; };

static void check(HRESULT result, const char *operation)
{
    if (FAILED(result)) throw RtgiError{result, operation};
}

// MSVC's std::isfinite is an out-of-line CRT call; this one runs per vertex.
static bool finite(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return (bits & 0x7f800000u) != 0x7f800000u;
}

static double elapsedMs(const LARGE_INTEGER &start)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return 1000.0 * double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart);
}

// Adds the scope's CPU time to total; a null total times nothing.
struct CpuClock {
    double *total;
    LARGE_INTEGER start{};
    explicit CpuClock(double *sum) : total(sum) { if (total) QueryPerformanceCounter(&start); }
    ~CpuClock() { if (total) *total += elapsedMs(start); }
};

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
    ComPtr<ID3D12Resource> uvs, triangles, materials, normals, motions, tints;
    ComPtr<ID3D12Resource> bottom, top, scratch;
    UINT64 completion = 0;
    bool timed = false;  // its D3D12 timestamps await reading
};

struct RtgiCacheSlice {
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11RenderTargetView> target;
    UINT64 used = UINT64_MAX;
    bool dirty = false, mutableSource = false;
};

struct RtgiMaterial {
    uint32_t slice, test;
    float alpha, factor;
    uint32_t colorSlice = opaqueMaterial;
};
static_assert(sizeof(RtgiMaterial) == 20, "HLSL material layout");

struct CompositeConstants {
    float fullSize[2], strength, split, traceSize[2], texelPixels;
    uint32_t reproject;
    XMFLOAT4X4 reprojection;
};
static_assert(sizeof(CompositeConstants) == 96, "HLSL composite layout");

// D3D11 timestamps around reduction, D3D12 wait and composite; read when reused.
struct RtgiTimer11 {
    ComPtr<ID3D11Query> disjoint, stamps[4];
    bool pending = false;
};
constexpr UINT stampsPerSlot = 5, timers11 = 6;

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
    // composite: single-sample, then the two per-pixel MSAA passes.
    ComPtr<ID3D11PixelShader> reduce[2], composite[3];
    ComPtr<ID3D11PixelShader> copyAlpha, copyColor, reduceColor;
    ComPtr<ID3D11SamplerState> linear;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11Buffer> constants11;
    RtgiTexture depth, color, gi, raw, filtered, motion, history[2], position[2], normal[2];
    RtgiTexture alpha, albedo;
    RtgiCacheSlice alphaCache[alphaSlices], colorCache[alphaSlices];
    RtgiSlot slots[3];
    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT3> normals;  // skinned vertex normals; zero when the draw has none
    // World motion since the previous frame, so moving meshes keep their GI history.
    // A mesh is its guest vertex buffer, shape and order among same-buffer draws.
    using MeshKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    std::vector<XMFLOAT3> motions, previousPositions;
    std::map<MeshKey, size_t> meshes, previousMeshes;
    std::vector<uint32_t> indices;
    // A frame whose triangles match the last traced frame's refits that BLAS.
    std::vector<uint32_t> previousIndices;
    RtgiSlot *lastBuilt = nullptr;
    UINT refits = 0;
    std::vector<XMFLOAT2> uvs;
    std::vector<XMFLOAT3> tints;
    std::vector<uint32_t> triangles;
    std::vector<RtgiMaterial> materials;
    XMFLOAT4X4 camera{}, previousCamera{};
    size_t cameraTriangles = 0;
    UINT width = 0, height = 0, scale = 2, traceWidth = 0, traceHeight = 0;
    UINT64 frame = 0, readyValue = 0, doneValue = 0;
    UINT64 capture = 0;
    float strength = 1;
    bool hitColor = true;
    bool failed = false, historyValid = false, alphaFullLogged = false, colorFullLogged = false;
    bool timing = false;
    // RECOMP_D3D_RTGI_HZ: a capture starting sooner than reuseMs after the last
    // traced one, such as a display pass between 60 Hz ticks, reprojects its GI.
    double reuseMs = 0;
    LARGE_INTEGER lastTrace{}, captureStart{};
    bool captureOpen = false, reuse = false;
    size_t reuseDraws = 0;
    ComPtr<ID3D12QueryHeap> stamps12;
    ComPtr<ID3D12Resource> stampReadback;
    double tickMs12 = 0;
    RtgiTimer11 timer11[timers11];
    UINT64 passes = 0;
    D3dRtgiStats stats;
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

static void texture(D3dRtgi &r, RtgiTexture &t, DXGI_FORMAT format, bool shared, bool renderTarget,
    UINT width = 0, UINT height = 0, UINT layers = 1)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width ? width : r.traceWidth;
    desc.Height = height ? height : r.traceHeight;
    desc.DepthOrArraySize = UINT16(layers);
    desc.MipLevels = desc.SampleDesc.Count = 1;
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
    if (renderTarget && layers == 1)
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
Texture2D<float> halfDepth : register(t3);
SamplerState linearClamp : register(s0);
cbuffer Frame : register(b0) {
    float2 fullSize; float strength; float split;
    float2 traceSize; float texelPixels; uint reproject;
    row_major float4x4 reprojection; // this frame's clip space to the traced frame's
};
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vs(uint id : SV_VertexID) {
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
float4 colorPS(Vertex v) : SV_Target { return colorImage.SampleLevel(linearClamp, v.uv, 0); }
float alphaPS(Vertex v) : SV_Target { return colorImage.SampleLevel(linearClamp, v.uv, 0).a; }
struct Reduced { float depth : SV_Target0; float4 color : SV_Target1; };
Reduced reducePS(Vertex v) {
    int2 p = min(int2(v.uv * fullSize), int2(fullSize) - 1);
    Reduced output;
    output.depth = readDepth(p);
    output.color = colorImage.Load(int3(p, 0));
    return output;
}
float4 reduceColorPS(Vertex v) : SV_Target {
    return colorImage.Load(int3(min(int2(v.uv * fullSize), int2(fullSize) - 1), 0));
}
// Dual-source blend: target *= scale + add. Unchanged is exactly scale 1, add 0.
struct Gain { float4 add : SV_Target0; float4 scale : SV_Target1; };
Gain shade(Vertex v, int2 p, float depth, float slope) {
    Gain gain = {float4(0, 0, 0, 0), float4(1, 1, 1, 1)};
    if (depth >= 1 || v.uv.x < split) return gain;
    // The 8-bit target cannot exceed white: fade brightening as the brightest
    // channel nears 1 so bright walls keep their texture and hue.
    float3 base = colorImage.SampleLevel(linearClamp, v.uv, 0).rgb;
    float headroom = saturate(1 - max(base.r, max(base.g, base.b)));
    // A frame between traces reads the last traced GI where this surface was then.
    float2 uv = v.uv;
    float traced = depth;
    if (reproject) {
        float4 clip = mul(float4(v.uv * float2(2, -2) + float2(-1, 1), depth, 1), reprojection);
        if (clip.w <= 0) return gain;
        uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
        traced = clip.z / clip.w;
    }
    // Bilinear upsampling that skips traced texels from another surface,
    // so light behind a silhouette does not bleed across it.
    float2 at = uv * traceSize - 0.5;
    int2 corner = int2(floor(at));
    float2 f = at - corner;
    float3 sum = 0;
    float total = 0;
    bool agrees = false;
    // A traced texel sits up to two of its widths away, so allow the local
    // depth slope over that distance on steep or grazing surfaces.
    float tolerance = max(max(2e-5, 0.025 * (1 - traced)), 2 * texelPixels * slope);
    [unroll] for (int i = 0; i < 4; ++i) {
        int2 offset = int2(i & 1, i >> 1);
        int2 q = clamp(corner + offset, int2(0, 0), int2(traceSize) - 1);
        float other = halfDepth.Load(int3(q, 0));
        bool same = abs(other - traced) <= tolerance;
        agrees = agrees || same;
        float w = (offset.x ? f.x : 1 - f.x) * (offset.y ? f.y : 1 - f.y) * (same ? 1 : 1e-3);
        sum += w * giImage.Load(int3(q, 0)).rgb;
        total += w;
    }
    // A sliver narrower than a half-resolution texel has no light of its own;
    // leaving it unchanged avoids a stepped halo of the surface behind it.
    if (!agrees) return gain;
    float3 change = strength * sum / max(total, 1e-6);
    gain.add.rgb = saturate(change * headroom);
    gain.scale.rgb = saturate(1 + min(change, 0));
    // Relighting quantizes the main target again, after its original dither.
    // Keep the noise fixed in screen space and preserve exactly unchanged pixels.
    float3 multiplier = gain.add.rgb + gain.scale.rgb;
    if (any(multiplier != 1)) {
        uint2 pixel = uint2(p);
        uint b = ((pixel.x ^ pixel.y) & 1) * 8 + (pixel.y & 1) * 4
            + (((pixel.x ^ pixel.y) >> 1) & 1) * 2 + ((pixel.y >> 1) & 1);
        float noise = ((b + 0.5) / 16 - 0.5) / 255;
        multiplier = max(0, multiplier + noise / max(max(base.r, max(base.g, base.b)), 1.0 / 255));
        gain.add.rgb = max(multiplier - 1, 0);
        gain.scale.rgb = min(multiplier, 1);
    }
    return gain;
}
#if MULTISAMPLED
// Shading per sample costs the sample count in invocations. Shade per pixel
// instead: pass 0 lights the samples on sample 0's surface, and pass 1 lights
// the rest, such as the far side of a silhouette, with the first such depth.
Gain compositePS(Vertex v, out uint coverage : SV_Coverage) {
    int2 p = int2(v.position.xy);
    uint width, height, samples;
    depthImage.GetDimensions(width, height, samples);
    float first = depthImage.Load(p, 0), depth = first;
    uint near = 0;
    bool found = PASS == 0;
    for (uint s = 0; s < samples; ++s) {
        float other = depthImage.Load(p, s);
        bool same = abs(other - first) <= max(2e-5, 0.025 * (1 - first));
        near |= same ? 1u << s : 0;
        if (!found && !same) { depth = other; found = true; }
    }
    // Quad neighbours shade other surfaces in pass 1, so its slope is meaningless there.
    float slope = PASS == 0 ? abs(ddx(depth)) + abs(ddy(depth)) : 0;
    coverage = !found ? 0 : PASS == 0 ? near : ~near;
    if (!coverage) return (Gain)0;
    return shade(v, p, depth, slope);
}
#else
Gain compositePS(Vertex v) {
    int2 p = int2(v.position.xy);
    float depth = readDepth(p);
    return shade(v, p, depth, abs(ddx(depth)) + abs(ddy(depth)));
}
#endif
)";

static ComPtr<ID3DBlob> compile11(const char *entry, const char *target, bool msaa, int pass = 0)
{
    const D3D_SHADER_MACRO macros[] = {{"MULTISAMPLED", msaa ? "1" : "0"},
        {"PASS", pass ? "1" : "0"}, {nullptr, nullptr}};
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
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 20, 1, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 0, 0, 20};
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
    D3D12_STATIC_SAMPLER_DESC alphaSampler{};
    alphaSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    alphaSampler.AddressU = alphaSampler.AddressV = alphaSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    alphaSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    alphaSampler.MaxLOD = D3D12_FLOAT32_MAX;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &alphaSampler;
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
        desc.NumDescriptors = 4 * frameDescriptors;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(r.device12->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&slot.descriptors)), "create frame descriptors");
    }
    texture(r, r.depth, DXGI_FORMAT_R32_FLOAT, true, true);
    texture(r, r.color, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
    texture(r, r.gi, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false);
    texture(r, r.raw, DXGI_FORMAT_R16G16B16A16_FLOAT, false, false);
    texture(r, r.filtered, DXGI_FORMAT_R16G16B16A16_FLOAT, false, false);
    texture(r, r.motion, DXGI_FORMAT_R16G16B16A16_FLOAT, false, false);
    // ponytail: 256-square alpha is enough for cutouts; raise the size if fine nets lose coverage.
    texture(r, r.alpha, DXGI_FORMAT_R8_UNORM, true, true, alphaSize, alphaSize, alphaSlices);
    // ponytail: one 256-square mip is the color proxy; add mips if aliasing is visible.
    texture(r, r.albedo, DXGI_FORMAT_R8G8B8A8_UNORM, true, true, alphaSize, alphaSize, alphaSlices);
    for (UINT i = 0; i < alphaSlices; ++i) {
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8_UNORM;
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.FirstArraySlice = i;
        view.Texture2DArray.ArraySize = 1;
        check(device->CreateRenderTargetView(r.alpha.imported.Get(), &view,
            &r.alphaCache[i].target), "create alpha slice target");
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        check(device->CreateRenderTargetView(r.albedo.imported.Get(), &view,
            &r.colorCache[i].target), "create color slice target");
    }
    for (unsigned i = 0; i < 2; ++i) {
        texture(r, r.history[i], DXGI_FORMAT_R16G16B16A16_FLOAT, false, false);
        texture(r, r.position[i], DXGI_FORMAT_R32G32B32A32_FLOAT, false, false);
        texture(r, r.normal[i], DXGI_FORMAT_R16G16B16A16_FLOAT, false, false);
    }
    auto vs = compile11("vs", "vs_5_0", false);
    check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
        &r.fullscreen), "create fullscreen vertex shader");
    auto alpha = compile11("alphaPS", "ps_5_0", false);
    check(device->CreatePixelShader(alpha->GetBufferPointer(), alpha->GetBufferSize(), nullptr,
        &r.copyAlpha), "create alpha copy shader");
    auto color = compile11("colorPS", "ps_5_0", false);
    check(device->CreatePixelShader(color->GetBufferPointer(), color->GetBufferSize(), nullptr,
        &r.copyColor), "create color copy shader");
    auto reduceColor = compile11("reduceColorPS", "ps_5_0", false);
    check(device->CreatePixelShader(reduceColor->GetBufferPointer(), reduceColor->GetBufferSize(), nullptr,
        &r.reduceColor), "create color reduction shader");
    for (unsigned i = 0; i < 2; ++i) {
        auto reduce = compile11("reducePS", "ps_5_0", i != 0);
        check(device->CreatePixelShader(reduce->GetBufferPointer(), reduce->GetBufferSize(), nullptr,
            &r.reduce[i]), "create reduction shader");
    }
    for (unsigned i = 0; i < 3; ++i) {
        auto composite = compile11("compositePS", "ps_5_0", i != 0, i == 2);
        check(device->CreatePixelShader(composite->GetBufferPointer(), composite->GetBufferSize(), nullptr,
            &r.composite[i]), "create composite shader");
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    check(device->CreateSamplerState(&sampler, &r.linear), "create bilinear sampler");
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(CompositeConstants);
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    // RECOMP_D3D_RTGI_SPLIT=1 leaves the left half unlit for side-by-side comparison.
    const char *split = std::getenv("RECOMP_D3D_RTGI_SPLIT");
    recomp_tuning.rtgi_strength = r.strength;
    recomp_tuning.rtgi_split = split && *split == '1';
    check(device->CreateBuffer(&constants, nullptr, &r.constants11), "create composite constants");
    D3D11_BLEND_DESC blend{};
    auto &target = blend.RenderTarget[0];
    target.BlendEnable = true;
    target.SrcBlend = D3D11_BLEND_DEST_COLOR;
    target.DestBlend = D3D11_BLEND_SRC1_COLOR;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ZERO;
    target.DestBlendAlpha = D3D11_BLEND_ONE;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
        D3D11_COLOR_WRITE_ENABLE_BLUE;
    check(device->CreateBlendState(&blend, &r.blend), "create GI blend state");
    if (!r.timing) return;
    D3D12_QUERY_HEAP_DESC heapDesc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 3 * stampsPerSlot, 0};
    check(r.device12->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&r.stamps12)), "create timestamp heap");
    D3D12_HEAP_PROPERTIES readback{};
    readback.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 3 * stampsPerSlot * sizeof(UINT64);
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(r.device12->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r.stampReadback)), "create timestamp readback");
    UINT64 frequency = 0;
    check(r.queue->GetTimestampFrequency(&frequency), "query D3D12 timestamp frequency");
    r.tickMs12 = 1000.0 / double(frequency);
    for (auto &timer : r.timer11) {
        D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        check(device->CreateQuery(&query, &timer.disjoint), "create disjoint query");
        query.Query = D3D11_QUERY_TIMESTAMP;
        for (auto &stamp : timer.stamps) check(device->CreateQuery(&query, &stamp), "create timestamp query");
    }
}

D3dRtgi *d3d_rtgi_create(ID3D11Device *device, uint32_t width, uint32_t height, bool timing)
{
    try {
        auto r = std::make_unique<D3dRtgi>();
        r->width = width;
        r->height = height;
        r->timing = timing;
        auto setting = [](const char *name, float fallback, float low, float high) {
            const char *text = std::getenv(name);
            char *end = nullptr;
            const float value = text ? std::strtof(text, &end) : fallback;
            return text && (end == text || *end != '\0' || !std::isfinite(value))
                ? fallback : std::clamp(value, low, high);
        };
        r->strength = setting("RECOMP_D3D_RTGI_STRENGTH", 1, 0, 4);
        // Trace one ray per scale x scale pixels; the composite upsamples by depth.
        r->scale = UINT(setting("RECOMP_D3D_RTGI_SCALE", 2, 1, 4));
        r->traceWidth = (width + r->scale - 1) / r->scale;
        r->traceHeight = (height + r->scale - 1) / r->scale;
        // 0.75 of the period, so steady 60 Hz frames always trace despite jitter.
        const float hz = setting("RECOMP_D3D_RTGI_HZ", 60, 0, 1000);
        r->reuseMs = hz > 0 ? 750.0 / hz : 0;
        const char *hitColor = std::getenv("RECOMP_D3D_RTGI_HITCOLOR");
        r->hitColor = !hitColor || std::strcmp(hitColor, "0") != 0;
        initialize(*r, device);
        char rate[32] = "every frame";
        if (hz > 0) std::snprintf(rate, sizeof rate, "up to %g Hz", hz);
        std::fprintf(stderr, "recomp rtgi: tracing %ux%u (1/%u scale) %s\n",
            r->traceWidth, r->traceHeight, r->scale, rate);
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

D3dRtgiStats d3d_rtgi_take_stats(D3dRtgi *r)
{
    if (!r) return {};
    const D3dRtgiStats stats = r->stats;
    r->stats = {};
    return stats;
}

static void stamp12(D3dRtgi &r, ID3D12GraphicsCommandList *commands, UINT index)
{
    if (r.stamps12) commands->EndQuery(r.stamps12.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        UINT(r.frame % 3) * stampsPerSlot + index);
}

// The slot's GPU work has completed, so its timestamps can be read without waiting.
static void readStamps12(D3dRtgi &r, RtgiSlot &slot)
{
    if (!slot.timed) return;
    slot.timed = false;
    const size_t first = size_t(&slot - r.slots) * stampsPerSlot * sizeof(UINT64);
    const D3D12_RANGE range{first, first + stampsPerSlot * sizeof(UINT64)}, none{0, 0};
    void *mapped = nullptr;
    if (FAILED(r.stampReadback->Map(0, &range, &mapped))) return;
    UINT64 t[stampsPerSlot];
    std::memcpy(t, static_cast<const char *>(mapped) + first, sizeof t);
    r.stampReadback->Unmap(0, &none);
    for (UINT i = 1; i < stampsPerSlot; ++i) if (t[i] < t[i - 1]) return;
    r.stats.buildMs += (t[1] - t[0]) * r.tickMs12;
    r.stats.traceMs += (t[2] - t[1]) * r.tickMs12;
    r.stats.temporalMs += (t[3] - t[2]) * r.tickMs12;
    r.stats.spatialMs += (t[4] - t[3]) * r.tickMs12;
    ++r.stats.d3d12Passes;
}

// Drops, without waiting, a timer whose results are not ready yet.
static void readTimer11(D3dRtgi &r, RtgiTimer11 &timer)
{
    if (!timer.pending) return;
    timer.pending = false;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
    UINT64 t[4];
    constexpr UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    if (r.context11->GetData(timer.disjoint.Get(), &frequency, sizeof frequency, flags) != S_OK ||
        frequency.Disjoint || !frequency.Frequency) return;
    for (UINT i = 0; i < 4; ++i)
        if (r.context11->GetData(timer.stamps[i].Get(), &t[i], sizeof t[i], flags) != S_OK ||
            (i && t[i] < t[i - 1])) return;
    const double tick = 1000.0 / double(frequency.Frequency);
    r.stats.compositeMs += double(t[1] - t[0] + t[3] - t[2]) * tick;
    r.stats.syncMs += double(t[2] - t[1]) * tick;
    ++r.stats.d3d11Passes;
}

static uint32_t textureSlice(D3dRtgi &r, ID3D11ShaderResourceView *source, bool color)
{
    auto &cache = color ? r.colorCache : r.alphaCache;
    UINT oldest = alphaSlices;
    for (UINT i = 0; i < alphaSlices; ++i) {
        auto &slice = cache[i];
        if (slice.source.Get() == source) {
            slice.used = r.capture;
            slice.dirty |= slice.mutableSource;
            return i;
        }
        if (slice.used != r.capture && (oldest == alphaSlices || !slice.source ||
            (cache[oldest].source && slice.used < cache[oldest].used))) oldest = i;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    source->GetDesc(&view);
    if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return opaqueMaterial;
    // Only display-domain color formats; never decode an sRGB view into this UNORM signal.
    if (color && view.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        view.Format != DXGI_FORMAT_B8G8R8A8_UNORM && view.Format != DXGI_FORMAT_B8G8R8X8_UNORM &&
        view.Format != DXGI_FORMAT_BC1_UNORM && view.Format != DXGI_FORMAT_BC2_UNORM &&
        view.Format != DXGI_FORMAT_BC3_UNORM) return opaqueMaterial;
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    source->GetResource(&resource);
    if (FAILED(resource.As(&texture))) return opaqueMaterial;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.BindFlags & D3D11_BIND_RENDER_TARGET) return opaqueMaterial;
    if (oldest == alphaSlices) {
        bool &logged = color ? r.colorFullLogged : r.alphaFullLogged;
        if (!logged) std::fprintf(stderr, "recomp rtgi: %s cache full; excess draws use %s\n",
            color ? "color" : "alpha", color ? "contact fallback" : "opaque triangles");
        logged = true;
        return opaqueMaterial;
    }
    auto &slice = cache[oldest];
    slice.source = source;
    slice.used = r.capture;
    slice.dirty = true;
    slice.mutableSource = desc.Usage != D3D11_USAGE_IMMUTABLE;
    return oldest;
}

bool d3d_rtgi_add_draw(D3dRtgi *r, const RecompD3dPresenterDrawCommand &draw,
    ID3D11ShaderResourceView *texture, ID3D11ShaderResourceView *alphaMask, bool colorMaterial)
{
    const CpuClock clock(r && r->timing ? &r->stats.collectMs : nullptr);
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
    if (!r->captureOpen) {
        r->captureOpen = true;
        QueryPerformanceCounter(&r->captureStart);
        r->reuse = r->reuseMs > 0 && r->historyValid && elapsedMs(r->lastTrace) < r->reuseMs;
    }
    if (r->reuse) {
        // Only the camera is needed. Approximate the traced choice, the draw adding
        // most triangles, without skinning: double-sided draws add both windings.
        const size_t weight = size_t(draw.triangle_count) * (draw.cull_mode == RECOMP_D3D_CULL_NONE ? 2 : 1);
        if (weight > r->cameraTriangles) {
            r->cameraTriangles = weight;
            std::memcpy(&r->camera, draw.view_projection, sizeof r->camera);
        }
        ++r->reuseDraws;
        return true;
    }
    // Indexed draws often use a small range near the end of a shared vertex buffer.
    // Do not skin or upload the unrelated prefix for every draw using that buffer.
    auto indexAt = [&](uint32_t i) {
        uint16_t index;
        std::memcpy(&index, static_cast<const unsigned char *>(draw.index_bytes) + size_t(i) * 2, 2);
        return uint32_t(index);
    };
    uint32_t firstVertex = draw.vertex_count, lastVertex = 0;
    for (uint32_t i = 0; i < draw.index_count; ++i) {
        const uint32_t index = indexAt(i);
        if (index >= draw.vertex_count) continue;
        firstVertex = std::min(firstVertex, index);
        lastVertex = std::max(lastVertex, index);
    }
    if (firstVertex > lastVertex) return false;
    const uint32_t vertexCount = lastVertex - firstVertex + 1;
    const size_t base = r->positions.size(), firstIndex = r->indices.size(), firstMaterial = r->materials.size();
    try {
        bool alphaTest = draw.depth.alpha_test_enable && texture &&
            (!draw.has_alpha_mask || alphaMask) && layout.texcoord_count &&
            uint64_t(layout.texcoord_offset) + sizeof(XMFLOAT2) <= draw.vertex_stride &&
            unsigned(draw.depth.alpha_func) < 8 && draw.depth.alpha_ref <= 255;
        // The caller must classify the original stages/pixel shader, not just the host shader.
        colorMaterial &= r->hitColor && texture && draw.has_texture && layout.texcoord_count == 1 &&
            uint64_t(layout.texcoord_offset) + sizeof(XMFLOAT2) <= draw.vertex_stride &&
            !draw.has_reflection && !draw.four_tap_filter && !draw.has_alpha_mask &&
            !draw.use_texture_factor && !draw.blend.blend_enable && !draw.blend_weight_count &&
            !draw.texture.render_target && !draw.texture_is_backbuffer &&
            (!draw.texture.linear || (draw.texture.width && draw.texture.height)) &&
            (!draw.directional.enabled || (draw.directional.count <= 8 &&
                layout.normal_offset != RECOMP_D3D_FVF_ABSENT &&
                uint64_t(layout.normal_offset) + 12 <= draw.vertex_stride &&
                layout.diffuse_offset == RECOMP_D3D_FVF_ABSENT)) &&
            (layout.diffuse_offset == RECOMP_D3D_FVF_ABSENT ||
                uint64_t(layout.diffuse_offset) + 4 <= draw.vertex_stride);
        // The presenter also admits point lights; this proxy evaluates only directional lights.
        if (colorMaterial && draw.directional.enabled)
            for (uint32_t i = 0; i < draw.directional.count; ++i)
                colorMaterial &= draw.directional.positions[i][3] == 0;
        // ponytail: reflection and four-tap alpha need more than UV0; keep those draws opaque.
        alphaTest &= !draw.has_reflection && !draw.four_tap_filter;
        const auto &source = draw.has_alpha_mask ? draw.alpha_mask : draw.texture;
        const XMFLOAT2 scale{source.linear && source.width ? 1.0f / source.width : 1.0f,
            source.linear && source.height ? 1.0f / source.height : 1.0f};
        // ponytail: CPU skinning is sufficient here; move to compute above roughly 1M vertices/frame.
        // Presized outputs and local copies of the draw's inputs: per-vertex push_back and
        // reloading draw fields after every aliasing store cost more than the skinning.
        r->positions.resize(base + vertexCount);
        r->normals.resize(base + vertexCount);
        r->uvs.resize(base + vertexCount);
        r->tints.resize(base + vertexCount, XMFLOAT3{1, 1, 1});
        XMFLOAT3 *const positions = r->positions.data() + base, *const normals = r->normals.data() + base;
        XMFLOAT2 *const uvs = r->uvs.data() + base;
        XMFLOAT3 *const tints = r->tints.data() + base;
        const uint32_t weightCount = draw.blend_weight_count, stride = draw.vertex_stride;
        const uint32_t normalOffset = layout.normal_offset, texcoordOffset = layout.texcoord_offset;
        const bool hasNormal = normalOffset != RECOMP_D3D_FVF_ABSENT && uint64_t(normalOffset) + 12 <= stride;
        float world[4][16];
        std::memcpy(world, draw.world, (weightCount + 1) * sizeof world[0]);
        const auto *vertexBytes = static_cast<const unsigned char *>(draw.vertex_bytes) + size_t(firstVertex) * stride;
        for (uint32_t v = 0; v < vertexCount; ++v) {
            const auto *bytes = vertexBytes + size_t(v) * stride;
            float p[3], weights[3]{};
            std::memcpy(p, bytes, sizeof p);
            // Fixed-size copies inline; a variable-length memcpy is a CRT call per vertex.
            for (uint32_t i = 0; i < weightCount; ++i) std::memcpy(&weights[i], bytes + 12 + 4 * i, sizeof(float));
            float n[3]{};
            if (hasNormal) std::memcpy(n, bytes + normalOffset, sizeof n);
            XMFLOAT3 transformed{}, normal{};
            float remainder = 1;
            for (uint32_t i = 0; i <= weightCount; ++i) {
                const float weight = i == weightCount ? remainder : weights[i];
                if (i < weightCount) remainder -= weight;
                const float *m = world[i];
                transformed.x += weight * (p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12]);
                transformed.y += weight * (p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13]);
                transformed.z += weight * (p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14]);
                // ponytail: the world 3x3 stands in for its inverse transpose; exact for rotation and uniform scale.
                normal.x += weight * (n[0] * m[0] + n[1] * m[4] + n[2] * m[8]);
                normal.y += weight * (n[0] * m[1] + n[1] * m[5] + n[2] * m[9]);
                normal.z += weight * (n[0] * m[2] + n[1] * m[6] + n[2] * m[10]);
            }
            if (!finite(transformed.x) || !finite(transformed.y) || !finite(transformed.z)) {
                r->positions.resize(base);
                r->normals.resize(base);
                r->uvs.resize(base);
                r->tints.resize(base);
                return false;
            }
            positions[v] = transformed;
            if (!finite(normal.x) || !finite(normal.y) || !finite(normal.z)) normal = {};
            normals[v] = normal;
            if (alphaTest || colorMaterial) {
                XMFLOAT2 uv;
                std::memcpy(&uv, bytes + texcoordOffset, sizeof uv);
                uv.x *= scale.x;
                uv.y *= scale.y;
                const bool usable = finite(uv.x) && finite(uv.y);
                alphaTest &= usable;
                colorMaterial &= usable;
                uvs[v] = uv;
            }
            if (colorMaterial) {
                XMFLOAT3 tint{1, 1, 1};
                if (layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT) {
                    uint32_t diffuse;
                    std::memcpy(&diffuse, bytes + layout.diffuse_offset, sizeof diffuse);
                    tint = {((diffuse >> 16) & 255) / 255.0f,
                        ((diffuse >> 8) & 255) / 255.0f, (diffuse & 255) / 255.0f};
                } else if (draw.directional.enabled) {
                    const auto &light = draw.directional;
                    auto ln = DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(
                        reinterpret_cast<const XMFLOAT3 *>(n)), DirectX::XMLoadFloat4x4(
                        reinterpret_cast<const XMFLOAT4X4 *>(light.normal_transforms[0])));
                    if (light.normalize && DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(ln)) > 0)
                        ln = DirectX::XMVector3Normalize(ln);
                    auto rgb = DirectX::XMLoadFloat3(reinterpret_cast<const XMFLOAT3 *>(light.ambient_emissive));
                    for (uint32_t i = 0; i < light.count; ++i) {
                        const float lambert = std::max(0.0f, DirectX::XMVectorGetX(DirectX::XMVector3Dot(ln,
                            DirectX::XMLoadFloat3(reinterpret_cast<const XMFLOAT3 *>(light.directions[i])))));
                        const auto diffuse = DirectX::XMVectorMultiply(
                            DirectX::XMLoadFloat3(reinterpret_cast<const XMFLOAT3 *>(light.material_diffuse)),
                            DirectX::XMLoadFloat3(reinterpret_cast<const XMFLOAT3 *>(light.colors[i])));
                        rgb = DirectX::XMVectorAdd(rgb, DirectX::XMVectorScale(diffuse, lambert));
                    }
                    DirectX::XMStoreFloat3(&tint, rgb);
                }
                colorMaterial &= std::isfinite(tint.x) && std::isfinite(tint.y) && std::isfinite(tint.z);
                tint = {std::clamp(tint.x, 0.0f, 1.0f), std::clamp(tint.y, 0.0f, 1.0f), std::clamp(tint.z, 0.0f, 1.0f)};
                if (draw.zero_diffuse_rgb) tint = {};
                if (draw.modulate_texture_factor) {
                    tint.x *= ((draw.texture_factor >> 16) & 255) / 255.0f;
                    tint.y *= ((draw.texture_factor >> 8) & 255) / 255.0f;
                    tint.z *= (draw.texture_factor & 255) / 255.0f;
                }
                tints[v] = tint;
            }
        }
        uint32_t material = opaqueMaterial;
        RtgiMaterial value{opaqueMaterial, 0, 1, 1};
        const auto address = [](uint32_t mode) { return mode == 2 ? 1u : mode == 3 || mode == 5 ? 2u : 0u; };
        const UINT u = draw.has_alpha_mask ? 2 : address(draw.address_u);
        const UINT v = draw.has_alpha_mask ? 2 : address(draw.address_v);
        value.test = (u << 11) | (v << 13);
        if (alphaTest) {
            // ponytail: vertex diffuse alpha is 1; upload it if vertex-faded cutouts need tracing.
            // The separate mask path bypasses material and texture-factor alpha in ps_main.
            if (!draw.has_alpha_mask) {
                value.alpha = draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_NONE
                    ? 1.0f : draw.material_alpha;
                if (draw.use_texture_factor || draw.modulate_texture_factor)
                    value.factor = ((draw.texture_factor >> 24) & 255) / 255.0f;
                if (draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE) value.test |= 1u << 15;
                if (draw.use_texture_factor) value.test |= 1u << 16;
            }
            value.test |= UINT(draw.depth.alpha_func) | (draw.depth.alpha_ref << 3);
            if (std::isfinite(value.alpha) && std::isfinite(value.factor))
                value.slice = textureSlice(*r, draw.has_alpha_mask ? alphaMask : texture, false);
        }
        // Border and mirror-once need different sampling; leave them unsupported for color.
        if (colorMaterial && draw.address_u <= 3 && draw.address_v <= 3)
            value.colorSlice = textureSlice(*r, texture, true);
        if (value.slice != opaqueMaterial || value.colorSlice != opaqueMaterial) {
            material = UINT(r->materials.size());
            r->materials.push_back(value);
        }
        const uint32_t triangles = std::min(draw.triangle_count,
            recomp_d3d_draw_triangle_count(draw.primitive_type, draw.index_count));
        const bool doubleSided = draw.cull_mode == RECOMP_D3D_CULL_NONE;
        const size_t firstTriangle = r->triangles.size(), most = size_t(triangles) * (doubleSided ? 2 : 1);
        if (firstIndex + most * 3 > UINT32_MAX) throw RtgiError{E_OUTOFMEMORY, "too many scene indices"};
        r->indices.resize(firstIndex + most * 3);
        r->triangles.resize(firstTriangle + most);
        uint32_t *outIndex = r->indices.data() + firstIndex, *outTriangle = r->triangles.data() + firstTriangle;
        const uint32_t vertexBase = uint32_t(base);
        for (uint32_t t = 0; t < triangles; ++t) {
            uint32_t a, b, c;
            if (draw.primitive_type == 5) { a = indexAt(t * 3); b = indexAt(t * 3 + 1); c = indexAt(t * 3 + 2); }
            else if (draw.primitive_type == 6) {
                a = indexAt(t); b = indexAt(t + 1); c = indexAt(t + 2);
                if (t & 1) std::swap(a, b);
            } else { a = indexAt(0); b = indexAt(t + 1); c = indexAt(t + 2); }
            if (a >= draw.vertex_count || b >= draw.vertex_count || c >= draw.vertex_count ||
                a == b || b == c || a == c) continue;
            a -= firstVertex; b -= firstVertex; c -= firstVertex;
            /* Keep zero-area triangles: DXR never intersects them, and dropping them
               would change the triangle list whenever skinning flattens one, which
               prevents refitting the last BLAS. */
            // Rays skip back faces, as DXR sees clockwise triangles from the front. Match
            // the raster: CCW-front draws flip, and double-sided draws get both windings.
            if (draw.cull_mode == RECOMP_D3D_CULL_CLOCKWISE) std::swap(b, c);
            outIndex[0] = vertexBase + a;
            outIndex[1] = vertexBase + b;
            outIndex[2] = vertexBase + c;
            outIndex += 3;
            *outTriangle++ = material;
            if (doubleSided) {
                outIndex[0] = vertexBase + a;
                outIndex[1] = vertexBase + c;
                outIndex[2] = vertexBase + b;
                outIndex += 3;
                *outTriangle++ = material;
            }
        }
        r->indices.resize(size_t(outIndex - r->indices.data()));
        r->triangles.resize(size_t(outTriangle - r->triangles.data()));
        const size_t contributed = (r->indices.size() - firstIndex) / 3;
        if (!contributed) {
            r->positions.resize(base);
            r->normals.resize(base);
            r->uvs.resize(base);
            r->tints.resize(base);
            r->materials.resize(firstMaterial);
        }
        if (contributed && draw.vertex_address) {
            D3dRtgi::MeshKey key{draw.vertex_address, vertexCount, draw.vertex_stride, firstVertex, 0};
            while (r->meshes.count(key)) ++std::get<4>(key);
            r->meshes[key] = base;
            r->motions.resize(r->positions.size());
            const auto previous = r->previousMeshes.find(key);
            if (previous != r->previousMeshes.end() &&
                previous->second + vertexCount <= r->previousPositions.size()) {
                for (uint32_t v = 0; v < vertexCount; ++v) {
                    const XMFLOAT3 &now = r->positions[base + v], &then = r->previousPositions[previous->second + v];
                    r->motions[base + v] = {now.x - then.x, now.y - then.y, now.z - then.z};
                }
            }
        }
        r->motions.resize(r->positions.size());
        if (contributed > r->cameraTriangles) {
            r->cameraTriangles = contributed;
            std::memcpy(&r->camera, draw.view_projection, sizeof r->camera);
        }
        return contributed != 0;
    } catch (const RtgiError &error) { fail(*r, error.operation, error.result); }
    catch (const std::exception &error) { fail(*r, error.what()); }
    r->positions.resize(base);
    r->normals.resize(base);
    r->uvs.resize(base);
    r->tints.resize(base);
    r->indices.resize(firstIndex);
    r->triangles.resize(firstIndex / 3);
    r->materials.resize(firstMaterial);
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
    upload(r, slot.normals, r.normals.data(), r.normals.size() * sizeof(XMFLOAT3));
    upload(r, slot.motions, r.motions.data(), r.motions.size() * sizeof(XMFLOAT3));
    upload(r, slot.indices, r.indices.data(), r.indices.size() * sizeof(uint32_t));
    upload(r, slot.tints, r.tints.data(), r.tints.size() * sizeof(XMFLOAT3));
    upload(r, slot.uvs, r.uvs.data(), r.uvs.size() * sizeof(XMFLOAT2));
    upload(r, slot.triangles, r.triangles.data(), r.triangles.size() * sizeof(uint32_t));
    const RtgiMaterial empty{};
    upload(r, slot.materials, r.materials.empty() ? &empty : r.materials.data(),
        std::max(size_t(1), r.materials.size()) * sizeof(RtgiMaterial));
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.VertexCount = UINT(r.positions.size());
    geometry.Triangles.VertexBuffer = {slot.vertices->GetGPUVirtualAddress(), sizeof(XMFLOAT3)};
    geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geometry.Triangles.IndexCount = UINT(r.indices.size());
    geometry.Triangles.IndexBuffer = slot.indices->GetGPUVirtualAddress();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};
    bottom.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottom.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD |
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
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
    ensureBuffer(r, slot.scratch, std::max({bottomSize.ScratchDataSizeInBytes,
        bottomSize.UpdateScratchDataSizeInBytes, topSize.ScratchDataSizeInBytes}), false);
    /* Moving skinned meshes keep their triangles, so refit the last BLAS: much
       cheaper than a build. Rebuild periodically, as refits loosen the BVH. */
    const bool refit = r.lastBuilt && r.refits < 30 && r.indices == r.previousIndices &&
        r.positions.size() == r.previousPositions.size();
    r.refits = refit ? r.refits + 1 : 0;
    if (refit) {
        bottom.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        if (r.timing) ++r.stats.refitted;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = bottom;
    build.DestAccelerationStructureData = slot.bottom->GetGPUVirtualAddress();
    if (refit) build.SourceAccelerationStructureData = r.lastBuilt->bottom->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = slot.scratch->GetGPUVirtualAddress();
    auto *commands = slot.commands.Get();
    // Reused result and scratch buffers were read/written by an earlier frame.
    uavBarrier(commands, nullptr);
    commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    uavBarrier(commands, slot.bottom.Get());
    uavBarrier(commands, slot.scratch.Get());
    build.Inputs = top;
    build.DestAccelerationStructureData = slot.top->GetGPUVirtualAddress();
    build.SourceAccelerationStructureData = 0;
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
    Constants constants{inverse, r.camera, r.previousCamera, r.traceWidth, r.traceHeight,
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
        r.normal[previous].resource.Get(), r.motion.resource.Get()};
    ID3D12Resource *writes[] = {r.raw.resource.Get(), r.history[current].resource.Get(),
        r.gi.resource.Get(), r.position[current].resource.Get(), r.normal[current].resource.Get(),
        r.motion.resource.Get()};
    auto descriptor = slot.descriptors->GetCPUDescriptorHandleForHeapStart();
    const UINT stride = r.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12Resource *filterInputs[] = {writes[1], r.filtered.resource.Get(), writes[0], r.filtered.resource.Get()};
    ID3D12Resource *filterOutputs[] = {r.filtered.resource.Get(), writes[0], r.filtered.resource.Get(), writes[2]};
    for (unsigned pass = 0; pass < 4; ++pass) {
        for (unsigned i = 0; i < 10; ++i) {
            auto *resource = i == 5 ? filterInputs[pass] : reads[i];
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = resource->GetDesc().Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            r.device12->CreateShaderResourceView(resource, &view, descriptor);
            descriptor.ptr += stride;
        }
        ID3D12Resource *geometry[] = {slot.vertices.Get(), slot.indices.Get(), slot.uvs.Get(),
            slot.triangles.Get(), slot.materials.Get(), slot.normals.Get(), slot.motions.Get()};
        const UINT counts[] = {UINT(r.positions.size()), UINT(r.indices.size()), UINT(r.uvs.size()),
            UINT(r.triangles.size()), UINT(std::max(size_t(1), r.materials.size())), UINT(r.normals.size()),
            UINT(r.motions.size())};
        const UINT sizes[] = {sizeof(XMFLOAT3), sizeof(uint32_t), sizeof(XMFLOAT2),
            sizeof(uint32_t), sizeof(RtgiMaterial), sizeof(XMFLOAT3), sizeof(XMFLOAT3)};
        for (unsigned i = 0; i < 7; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Buffer.NumElements = counts[i];
            view.Buffer.StructureByteStride = sizes[i];
            r.device12->CreateShaderResourceView(geometry[i], &view, descriptor);
            descriptor.ptr += stride;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC alpha{};
        alpha.Format = DXGI_FORMAT_R8_UNORM;
        alpha.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        alpha.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        alpha.Texture2DArray.MipLevels = 1;
        alpha.Texture2DArray.ArraySize = alphaSlices;
        r.device12->CreateShaderResourceView(r.alpha.resource.Get(), &alpha, descriptor);
        descriptor.ptr += stride;
        alpha.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        r.device12->CreateShaderResourceView(r.albedo.resource.Get(), &alpha, descriptor);
        descriptor.ptr += stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC tint{};
        tint.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        tint.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tint.Buffer.NumElements = UINT(r.tints.size());
        tint.Buffer.StructureByteStride = sizeof(XMFLOAT3);
        r.device12->CreateShaderResourceView(slot.tints.Get(), &tint, descriptor);
        descriptor.ptr += stride;
        for (unsigned i = 0; i < 6; ++i) {
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
    transition(commands, r.alpha.resource.Get(), common, srv);
    transition(commands, r.albedo.resource.Get(), common, srv);
    for (unsigned i : {0u, 1u, 3u, 4u, 5u}) transition(commands, writes[i], common, uav);
    commands->SetComputeRootSignature(r.root.Get());
    commands->SetDescriptorHeaps(1, slot.descriptors.GetAddressOf());
    commands->SetComputeRootShaderResourceView(0, slot.top->GetGPUVirtualAddress());
    commands->SetComputeRootConstantBufferView(1, slot.constants->GetGPUVirtualAddress());
    commands->SetComputeRootDescriptorTable(2, slot.descriptors->GetGPUDescriptorHandleForHeapStart());
    const UINT x = (r.traceWidth + 7) / 8, y = (r.traceHeight + 7) / 8;
    commands->SetPipelineState(r.trace.Get());
    commands->Dispatch(x, y, 1);
    transition(commands, writes[0], uav, srv);
    transition(commands, writes[3], uav, srv);
    transition(commands, writes[4], uav, srv);
    transition(commands, writes[5], uav, srv);
    stamp12(r, commands, 2);
    commands->SetPipelineState(r.temporal.Get());
    commands->Dispatch(x, y, 1);
    transition(commands, writes[1], uav, srv);
    stamp12(r, commands, 3);
    commands->SetPipelineState(r.spatial.Get());
    auto table = slot.descriptors->GetGPUDescriptorHandleForHeapStart();
    for (unsigned pass = 0; pass < 4; ++pass) {
        // raw is no longer needed by temporal; reuse it as a filter scratch image.
        transition(commands, filterOutputs[pass], pass == 0 || pass == 3 ? common : srv, uav);
        commands->SetComputeRootDescriptorTable(2, table);
        commands->SetComputeRoot32BitConstant(3, 1u << pass, 0);
        commands->Dispatch(x, y, 1);
        transition(commands, filterOutputs[pass], uav, pass == 3 ? common : srv);
        table.ptr += frameDescriptors * stride;
    }
    stamp12(r, commands, 4);
    for (auto *resource : reads) transition(commands, resource, srv, common);
    transition(commands, r.filtered.resource.Get(), srv, common);
    transition(commands, r.alpha.resource.Get(), srv, common);
    transition(commands, r.albedo.resource.Get(), srv, common);
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

// reprojection: null for a traced frame, else this frame's clip space to the traced one's.
static void setConstants(D3dRtgi &r, ID3D11DeviceContext *context, const XMFLOAT4X4 *reprojection)
{
    // Hotkeys can change strength and split between frames.
    const CompositeConstants values{{float(r.width), float(r.height)}, recomp_tuning.rtgi_strength.load(),
        recomp_tuning.rtgi_split ? 0.5f : 0.0f, {float(r.traceWidth), float(r.traceHeight)}, float(r.scale),
        reprojection != nullptr, reprojection ? *reprojection : XMFLOAT4X4{}};
    context->UpdateSubresource(r.constants11.Get(), 0, nullptr, &values, 0, 0);
}

static void composite(D3dRtgi &r, ID3D11DeviceContext *context, ID3D11RenderTargetView *target,
    ID3D11ShaderResourceView *depth, uint32_t depthSamples)
{
    // The reduced color stands in for the target, which cannot be read while bound.
    ID3D11ShaderResourceView *inputs[] = {r.color.srv.Get(), depth, r.gi.srv.Get(), r.depth.srv.Get()};
    for (UINT pass = depthSamples > 1 ? 1 : 0; pass < (depthSamples > 1 ? 3u : 1u); ++pass)
        fullscreen(r, context, r.composite[pass].Get(), r.width, r.height, inputs, 4, &target, 1, r.blend.Get());
}

static bool invert(const XMFLOAT4X4 &matrix, XMFLOAT4X4 &inverse)
{
    DirectX::XMVECTOR determinant;
    DirectX::XMStoreFloat4x4(&inverse, DirectX::XMMatrixInverse(&determinant, DirectX::XMLoadFloat4x4(&matrix)));
    bool invertible = DirectX::XMVectorGetX(determinant) != 0;
    for (const auto &row : inverse.m) for (float value : row) invertible &= std::isfinite(value);
    return invertible;
}

static void traceAndComposite(D3dRtgi &r, ID3D11DeviceContext *context, ID3D11RenderTargetView *target,
    ID3D11ShaderResourceView *color, ID3D11ShaderResourceView *depth, uint32_t depthSamples,
    const XMFLOAT4X4 &inverse, RtgiTimer11 *timer)
{
    setConstants(r, context, nullptr);
    auto &slot = r.slots[r.frame % 3];
    {
        const CpuClock wait(r.timing ? &r.stats.waitMs : nullptr);
        waitFence(r, r.done12.Get(), slot.completion);
    }
    readStamps12(r, slot);
    check(slot.allocator->Reset(), "reset frame allocator");
    check(slot.commands->Reset(slot.allocator.Get(), nullptr), "reset frame commands");
    stamp12(r, slot.commands.Get(), 0);
    buildScene(r, slot);
    stamp12(r, slot.commands.Get(), 1);
    traceFrame(r, slot, inverse);
    if (r.stamps12) {
        const UINT first = UINT(r.frame % 3) * stampsPerSlot;
        slot.commands->ResolveQueryData(r.stamps12.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first,
            stampsPerSlot, r.stampReadback.Get(), first * sizeof(UINT64));
        slot.timed = true;
    }
    check(slot.commands->Close(), "close frame commands");

    if (timer) {
        context->Begin(timer->disjoint.Get());
        context->End(timer->stamps[0].Get());
    }
    for (bool color : {false, true}) for (UINT i = 0; i < alphaSlices; ++i) {
        auto &slice = color ? r.colorCache[i] : r.alphaCache[i];
        if (slice.used != r.capture || !slice.dirty) continue;
        fullscreen(r, context, color ? r.copyColor.Get() : r.copyAlpha.Get(), alphaSize, alphaSize,
            slice.source.GetAddressOf(), 1, slice.target.GetAddressOf(), 1);
        slice.dirty = false;
    }
    ID3D11ShaderResourceView *inputs[] = {color, depth};
    ID3D11RenderTargetView *targets[] = {r.depth.rtv.Get(), r.color.rtv.Get()};
    fullscreen(r, context, r.reduce[depthSamples > 1].Get(), r.traceWidth, r.traceHeight, inputs, 2, targets, 2);
    if (timer) context->End(timer->stamps[1].Get());
    const UINT64 inputValue = r.frame * 2 + 1, doneValue = r.frame + 1;
    check(r.context11->Signal(r.ready11.Get(), inputValue), "signal D3D11 input ready");
    r.readyValue = inputValue;
    context->Flush();
    check(r.queue->Wait(r.ready12.Get(), inputValue), "D3D12 wait for input");
    ID3D12CommandList *lists[] = {slot.commands.Get()};
    r.queue->ExecuteCommandLists(1, lists);
    check(r.queue->Signal(r.done12.Get(), doneValue), "signal GI ready");
    r.doneValue = slot.completion = doneValue;
    r.lastBuilt = &slot;
    check(r.context11->Wait(r.done11.Get(), doneValue), "D3D11 wait for GI");
    if (timer) context->End(timer->stamps[2].Get());
    composite(r, context, target, depth, depthSamples);
    if (timer) context->End(timer->stamps[3].Get());
    check(r.context11->Signal(r.ready11.Get(), inputValue + 1), "signal GI consumed");
    r.readyValue = inputValue + 1;
    context->Flush();
    check(r.device12->GetDeviceRemovedReason(), "D3D12 frame execution");
    check(r.device11->GetDeviceRemovedReason(), "D3D11 frame execution");
    r.previousCamera = r.camera;
    r.historyValid = true;
    r.lastTrace = r.captureStart;
    if (r.timing) {
        ++r.stats.traced;
        r.stats.triangles += r.indices.size() / 3;
    }
    ++r.frame;
}

// A frame between traces lights itself with the last traced GI, reprojected
// from that frame's camera. D3D12 is idle: its last work finished before the
// last composite, and the next trace waits for this frame's D3D11 work.
static void reuseAndComposite(D3dRtgi &r, ID3D11DeviceContext *context, ID3D11RenderTargetView *target,
    ID3D11ShaderResourceView *color, ID3D11ShaderResourceView *depth, uint32_t depthSamples,
    const XMFLOAT4X4 &inverse, RtgiTimer11 *timer)
{
    XMFLOAT4X4 reprojection;
    DirectX::XMStoreFloat4x4(&reprojection,
        DirectX::XMLoadFloat4x4(&inverse) * DirectX::XMLoadFloat4x4(&r.previousCamera));
    setConstants(r, context, &reprojection);
    if (timer) {
        context->Begin(timer->disjoint.Get());
        context->End(timer->stamps[0].Get());
    }
    // Refresh only the reduced color for headroom; the traced depth must match the GI.
    fullscreen(r, context, r.reduceColor.Get(), r.traceWidth, r.traceHeight, &color, 1, r.color.rtv.GetAddressOf(), 1);
    if (timer) {
        context->End(timer->stamps[1].Get());
        context->End(timer->stamps[2].Get());
    }
    composite(r, context, target, depth, depthSamples);
    if (timer) context->End(timer->stamps[3].Get());
    if (r.timing) ++r.stats.reused;
}

void d3d_rtgi_apply(D3dRtgi *r, ID3D11DeviceContext *context, ID3D11RenderTargetView *target,
    ID3D11ShaderResourceView *color, ID3D11ShaderResourceView *depth, uint32_t depthSamples)
{
    const CpuClock clock(r && r->timing ? &r->stats.applyMs : nullptr);
    bool applied = false;
    const bool recorded = r && (r->reuse ? r->reuseDraws != 0 : !r->indices.empty());
    if (recorded && !r->failed && context && target && color && depth) {
        try {
            if (context != r->context11.Get()) throw RtgiError{E_INVALIDARG, "unexpected D3D11 context"};
            check(r->device12->GetDeviceRemovedReason(), "D3D12 device removed");
            check(r->device11->GetDeviceRemovedReason(), "D3D11 device removed");
            XMFLOAT4X4 inverse;
            if (invert(r->camera, inverse)) {
                RtgiTimer11 *timer = r->timing ? &r->timer11[r->passes++ % timers11] : nullptr;
                if (timer) readTimer11(*r, *timer);
                if (r->reuse) reuseAndComposite(*r, context, target, color, depth, depthSamples, inverse, timer);
                else traceAndComposite(*r, context, target, color, depth, depthSamples, inverse, timer);
                if (timer) {
                    context->End(timer->disjoint.Get());
                    timer->pending = true;
                }
                applied = true;
            }
        } catch (const RtgiError &error) { fail(*r, error.operation, error.result); }
        catch (const std::exception &error) { fail(*r, error.what()); }
    }
    if (r) {
        // Motion and history stay relative to the last traced frame.
        if (!r->reuse) {
            r->previousPositions.swap(r->positions);
            r->previousMeshes.swap(r->meshes);
            r->previousIndices.swap(r->indices);
            if (!applied) r->lastBuilt = nullptr;
        }
        r->meshes.clear();
        r->motions.clear();
        r->positions.clear();
        r->normals.clear();
        r->indices.clear();
        r->uvs.clear();
        r->tints.clear();
        r->triangles.clear();
        r->materials.clear();
        ++r->capture;
        r->cameraTriangles = 0;
        r->captureOpen = r->reuse = false;
        r->reuseDraws = 0;
        if (!applied) r->historyValid = false;
    }
    if (context) context->ClearState();
}
