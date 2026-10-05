#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <vector>

#include "dxr_smoke_shader.h"
#include "dxr_image_shader.h"

using Microsoft::WRL::ComPtr;

static void check(HRESULT result, const char *operation)
{
    if (FAILED(result)) {
        std::fprintf(stderr, "FAIL %s: hr=0x%08lX\n", operation,
            static_cast<unsigned long>(result));
        throw result;
    }
}

static bool checkD3D11Messages(ID3D11InfoQueue *messages)
{
    bool passed = true;
    if (messages == nullptr) return passed;
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0;
        check(messages->GetMessage(i, nullptr, &size), "size D3D11 debug message");
        std::vector<unsigned char> storage(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
        check(messages->GetMessage(i, message, &size), "read D3D11 debug message");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            std::fprintf(stderr, "D3D11: %s\n", message->pDescription);
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) passed = false;
        }
    }
    return passed;
}

static ComPtr<ID3D12Resource> buffer(ID3D12Device *device, UINT64 size,
    D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_STATES state,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        state, nullptr, IID_PPV_ARGS(&resource)), "create buffer");
    return resource;
}

static ComPtr<ID3D12Resource> upload(ID3D12Device *device, const void *data, size_t size)
{
    auto resource = buffer(device, size, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    check(resource->Map(0, &no_read, &mapped), "map upload");
    std::memcpy(mapped, data, size);
    resource->Unmap(0, nullptr);
    return resource;
}

static bool runInterop(ID3D12Device *device12, IDXGIAdapter1 *adapter,
    ID3D12CommandQueue *queue, D3D12_GPU_VIRTUAL_ADDRESS scene, bool warp = false)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    const auto driver = adapter ? D3D_DRIVER_TYPE_UNKNOWN :
        (warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE);
    HRESULT created = D3D11CreateDevice(adapter, driver, nullptr,
        D3D11_CREATE_DEVICE_DEBUG, levels, 3, D3D11_SDK_VERSION, &device, nullptr, &context);
    const bool debug_enabled = created != DXGI_ERROR_SDK_COMPONENT_MISSING;
    if (!debug_enabled) created = D3D11CreateDevice(adapter, driver, nullptr,
        0, levels, 3, D3D11_SDK_VERSION, &device, nullptr, &context);
    check(created, "create D3D11 device");
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> actual_adapter;
    check(device.As(&dxgi_device), "get D3D11 DXGI device");
    check(dxgi_device->GetAdapter(&actual_adapter), "get D3D11 adapter");
    DXGI_ADAPTER_DESC adapter_desc{};
    check(actual_adapter->GetDesc(&adapter_desc), "describe D3D11 adapter");
    std::printf("D3D11 adapter: %ls; debug layer: %s; DXR: %s\n",
        adapter_desc.Description, debug_enabled ? "enabled" : "unavailable",
        device12 ? "enabled" : "disabled (no D3D12 initialization)");
    if (device12) {
        const LUID luid = device12->GetAdapterLuid();
        if (luid.LowPart != adapter_desc.AdapterLuid.LowPart ||
            luid.HighPart != adapter_desc.AdapterLuid.HighPart)
            throw std::runtime_error("D3D11 and D3D12 adapters differ");
    }
    ComPtr<ID3D11InfoQueue> messages;
    if (debug_enabled) check(device.As(&messages), "get D3D11 debug messages");

    static const char shader[] = R"(
        Texture2D<float4> source : register(t0);
        SamplerState pointSampler : register(s0);
        cbuffer Frame : register(b0) { uint frameIndex; };
        struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
        Vertex vs(uint id : SV_VertexID) {
            Vertex v;
            v.uv = float2((id << 1) & 2, id & 2);
            v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
            return v;
        }
        float4 sharedPS(Vertex v) : SV_Target {
            return source.SampleLevel(pointSampler, v.uv, 0);
        }
        float4 fallbackPS(Vertex v) : SV_Target {
            uint2 pixel = uint2(v.position.xy);
            uint stamp = (frameIndex * 17 + pixel.x + pixel.y * 3) & 255;
            return float4(32, 64, stamp, 255) / 255.0;
        }
    )";
    ComPtr<ID3DBlob> vertex_blob, pixel_blob, errors;
    check(D3DCompile(shader, sizeof shader - 1, nullptr, nullptr, nullptr,
        "vs", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertex_blob, &errors),
        "compile D3D11 vertex shader");
    errors.Reset();
    check(D3DCompile(shader, sizeof shader - 1, nullptr, nullptr, nullptr,
        device12 ? "sharedPS" : "fallbackPS", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS,
        0, &pixel_blob, &errors), "compile D3D11 pixel shader");
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    check(device->CreateVertexShader(vertex_blob->GetBufferPointer(),
        vertex_blob->GetBufferSize(), nullptr, &vertex_shader), "create D3D11 vertex shader");
    check(device->CreatePixelShader(pixel_blob->GetBufferPointer(),
        pixel_blob->GetBufferSize(), nullptr, &pixel_shader), "create D3D11 pixel shader");
    ComPtr<ID3D11SamplerState> sampler;
    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    check(device->CreateSamplerState(&sampler_desc, &sampler), "create D3D11 sampler");
    ComPtr<ID3D11Buffer> constants;
    D3D11_BUFFER_DESC constants_desc{};
    constants_desc.ByteWidth = 16;
    constants_desc.Usage = D3D11_USAGE_DYNAMIC;
    constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check(device->CreateBuffer(&constants_desc, nullptr, &constants), "create D3D11 constants");

    ComPtr<ID3D11Device5> sharing_device;
    ComPtr<ID3D11DeviceContext4> sharing_context;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    if (device12) {
        check(device.As(&sharing_device), "get D3D11 fence device");
        check(context.As(&sharing_context), "get D3D11 fence context");
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 1;
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = {1, &range};
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants.Num32BitValues = 3;
        D3D12_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.NumParameters = 3;
        root_desc.pParameters = parameters;
        ComPtr<ID3DBlob> blob;
        errors.Reset();
        check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
            &blob, &errors), "serialize image root signature");
        check(device12->CreateRootSignature(0, blob->GetBufferPointer(),
            blob->GetBufferSize(), IID_PPV_ARGS(&root)), "create image root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
        pipeline_desc.pRootSignature = root.Get();
        pipeline_desc.CS = {kDxrImageShader, sizeof kDxrImageShader};
        check(device12->CreateComputePipelineState(&pipeline_desc,
            IID_PPV_ARGS(&pipeline)), "create image ray query pipeline");
    }

    bool passed = true;
    constexpr UINT frame_count = 8;
    const UINT sizes[][2] = {{8, 4}, {13, 5}};
    for (const auto &size : sizes) {
        const UINT width = size[0], height = size[1];
        ComPtr<ID3D12Resource> shared;
        ComPtr<ID3D12DescriptorHeap> descriptors;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands[frame_count];
        ComPtr<ID3D12Fence> fences12[2]; // Produced by DXR, consumed by D3D11.
        ComPtr<ID3D11Fence> fences11[2];
        ComPtr<ID3D11Texture2D> imported;
        ComPtr<ID3D11ShaderResourceView> source;
        if (device12) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            // Retain render-target capability for D3D11 shared-texture import.
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
            check(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED,
                &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&shared)),
                "create shared image");
            HANDLE handle = nullptr;
            check(device12->CreateSharedHandle(shared.Get(), nullptr, GENERIC_ALL,
                nullptr, &handle), "share image handle");
            const HRESULT opened = sharing_device->OpenSharedResource1(handle, IID_PPV_ARGS(&imported));
            CloseHandle(handle);
            if (FAILED(opened)) checkD3D11Messages(messages.Get());
            check(opened, "open shared image in D3D11");
            check(device->CreateShaderResourceView(imported.Get(), nullptr, &source),
                "create shared image SRV");
            for (unsigned i = 0; i < 2; ++i) {
                check(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                    IID_PPV_ARGS(&fences12[i])), "create shared fence");
                check(device12->CreateSharedHandle(fences12[i].Get(), nullptr,
                    GENERIC_ALL, nullptr, &handle), "share fence handle");
                const HRESULT fence_opened = sharing_device->OpenSharedFence(handle,
                    IID_PPV_ARGS(&fences11[i]));
                CloseHandle(handle);
                check(fence_opened, "open shared fence in D3D11");
            }
            D3D12_DESCRIPTOR_HEAP_DESC descriptors_desc{};
            descriptors_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            descriptors_desc.NumDescriptors = 1;
            descriptors_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            check(device12->CreateDescriptorHeap(&descriptors_desc,
                IID_PPV_ARGS(&descriptors)), "create image descriptor heap");
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = desc.Format;
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device12->CreateUnorderedAccessView(shared.Get(), nullptr, &view,
                descriptors->GetCPUDescriptorHandleForHeapStart());
            check(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&allocator)), "create image allocator");
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target, readbacks[frame_count];
        ComPtr<ID3D11RenderTargetView> target_view;
        check(device->CreateTexture2D(&desc, nullptr, &target), "create D3D11 target");
        check(device->CreateRenderTargetView(target.Get(), nullptr, &target_view), "create D3D11 RTV");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto &readback : readbacks)
            check(device->CreateTexture2D(&desc, nullptr, &readback), "create D3D11 readback");
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex_shader.Get(), nullptr, 0);
        context->PSSetShader(pixel_shader.Get(), nullptr, 0);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());

        for (UINT frame = 0; frame < frame_count; ++frame) {
            if (device12) {
                check(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&commands[frame])),
                    "create image command list");
                auto *command = commands[frame].Get();
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition = {shared.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
                command->ResourceBarrier(1, &barrier);
                command->SetComputeRootSignature(root.Get());
                command->SetDescriptorHeaps(1, descriptors.GetAddressOf());
                command->SetComputeRootShaderResourceView(0, scene);
                command->SetComputeRootDescriptorTable(1, descriptors->GetGPUDescriptorHandleForHeapStart());
                const UINT constants12[] = {frame, width, height};
                command->SetComputeRoot32BitConstants(2, 3, constants12, 0);
                command->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                command->ResourceBarrier(1, &barrier);
                check(command->Close(), "close image command list");
                if (frame != 0) check(queue->Wait(fences12[1].Get(), frame),
                    "wait for D3D11 consumption");
                ID3D12CommandList *lists[] = {command};
                queue->ExecuteCommandLists(1, lists);
                check(queue->Signal(fences12[0].Get(), frame + 1), "signal image ready");
                check(sharing_context->Wait(fences11[0].Get(), frame + 1), "wait for DXR image");
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                "map D3D11 constants");
            const UINT constants11[4] = {frame, 0, 0, 0};
            std::memcpy(mapped.pData, constants11, sizeof constants11);
            context->Unmap(constants.Get(), 0);
            context->OMSetRenderTargets(1, target_view.GetAddressOf(), nullptr);
            context->PSSetShaderResources(0, 1, source.GetAddressOf());
            context->Draw(3, 0);
            ID3D11ShaderResourceView *none = nullptr;
            context->PSSetShaderResources(0, 1, &none);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            context->CopyResource(readbacks[frame].Get(), target.Get());
            if (device12) check(sharing_context->Signal(fences11[1].Get(), frame + 1),
                "signal image consumed");
            context->Flush();
        }
        // Readbacks are test snapshots; the DX12-to-DX11 transfer stays on the GPU.
        ComPtr<ID3D11Query> complete;
        const D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
        check(device->CreateQuery(&query_desc, &complete), "create D3D11 completion query");
        context->End(complete.Get());
        context->Flush();
        const ULONGLONG started = GetTickCount64();
        HRESULT ready;
        do {
            ready = context->GetData(complete.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (ready != S_FALSE) break;
            Sleep(1);
        } while (GetTickCount64() - started < 10000);
        check(ready, "D3D11 completion query");
        check(device->GetDeviceRemovedReason(), "D3D11 image execution");
        if (ready != S_OK) throw std::runtime_error("D3D11 image test timed out after 10 seconds");
        if (device12 && (fences12[0]->GetCompletedValue() != frame_count ||
            fences12[1]->GetCompletedValue() != frame_count))
            throw std::runtime_error("shared fence did not complete every frame");
        bool pixels_ok = true;
        for (UINT frame = 0; frame < frame_count; ++frame) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(readbacks[frame].Get(), 0, D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped), "map D3D11 pixels");
            for (UINT y = 0; y < height; ++y) {
                const auto *row = static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch;
                for (UINT x = 0; x < width; ++x) {
                    const bool hit = x % 4 < 2;
                    const uint8_t expected[] = {
                        uint8_t(device12 ? (hit ? 255 : 0) : 32),
                        uint8_t(device12 ? (hit ? (x % 4 == 0 ? 7 : 11) : 0) : 64),
                        uint8_t((frame * 17 + x + y * 3) & 255), 255};
                    if (std::memcmp(row + x * 4, expected, 4) != 0) {
                        if (pixels_ok) std::fprintf(stderr,
                            "FAIL image=%ux%u frame=%u pixel=(%u,%u) got=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                            width, height, frame, x, y, row[x*4], row[x*4+1], row[x*4+2], row[x*4+3],
                            expected[0], expected[1], expected[2], expected[3]);
                        pixels_ok = false;
                    }
                }
            }
            context->Unmap(readbacks[frame].Get(), 0);
        }
        std::printf("%s %s %ux%u: %u frames, %u pixels verified\n", pixels_ok ? "PASS" : "FAIL",
            device12 ? "DXR -> D3D11" : "D3D11 only", width, height, frame_count,
            width * height * frame_count);
        passed = passed && pixels_ok;
    }
    context->ClearState();
    context->Flush();
    return checkD3D11Messages(messages.Get()) && passed;
}

static int run()
{
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
    }
    std::printf("D3D12 debug layer: %s\n", debug ? "enabled" : "unavailable");

    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create DXGI factory");
    ComPtr<ID3D12Device5> device;
    ComPtr<IDXGIAdapter1> selected_adapter;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumerated = factory->EnumAdapters1(i, &adapter);
        if (enumerated == DXGI_ERROR_NOT_FOUND) break;
        check(enumerated, "enumerate adapters");
        DXGI_ADAPTER_DESC1 desc{};
        check(adapter->GetDesc1(&desc), "describe adapter");
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ComPtr<ID3D12Device5> candidate;
        const HRESULT created = D3D12CreateDevice(adapter.Get(),
            D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&candidate));
        if (FAILED(created)) {
            std::printf("Adapter %ls: D3D12 unavailable (0x%08lX)\n",
                desc.Description, static_cast<unsigned long>(created));
            continue;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
        check(candidate->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
            &options, sizeof options), "query DXR support");
        D3D12_FEATURE_DATA_SHADER_MODEL shader_model{D3D_SHADER_MODEL_6_5};
        check(candidate->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
            &shader_model, sizeof shader_model), "query shader model");
        std::printf("Adapter %ls: DXR tier=%u shader model=0x%x\n",
            desc.Description, static_cast<unsigned>(options.RaytracingTier),
            static_cast<unsigned>(shader_model.HighestShaderModel));
        if (options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 &&
            shader_model.HighestShaderModel >= D3D_SHADER_MODEL_6_5) {
            device = candidate;
            selected_adapter = adapter;
            break;
        }
    }
    if (!device) {
        std::puts("SKIP: no hardware adapter with DXR 1.1 and Shader Model 6.5");
        return 77;
    }
    ComPtr<ID3D12InfoQueue> messages;
    if (debug) check(device.As(&messages), "get debug messages");

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "create queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&allocator)), "create allocator");
    ComPtr<ID3D12GraphicsCommandList4> commands;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "create command list");

    const float triangle[3][3] = {{0, 0, 1}, {1, 0, 1}, {0, 1, 1}};
    auto vertices = upload(device.Get(), triangle, sizeof triangle);
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.VertexCount = 3;
    geometry.Triangles.VertexBuffer = {vertices->GetGPUVirtualAddress(), sizeof triangle[0]};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom_inputs{};
    bottom_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottom_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bottom_inputs.NumDescs = 1;
    bottom_inputs.pGeometryDescs = &geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottom_sizes{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&bottom_inputs, &bottom_sizes);
    auto bottom = buffer(device.Get(), bottom_sizes.ResultDataMaxSizeInBytes,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto bottom_scratch = buffer(device.Get(), bottom_sizes.ScratchDataSizeInBytes,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_RAYTRACING_INSTANCE_DESC instances[2]{};
    for (auto &instance : instances) {
        instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1;
        instance.InstanceMask = 0xff;
        instance.AccelerationStructure = bottom->GetGPUVirtualAddress();
    }
    instances[0].InstanceID = 7;
    instances[1].InstanceID = 11;
    instances[1].Transform[0][3] = 2;
    instances[1].Transform[2][3] = 2;
    auto instance_buffer = upload(device.Get(), instances, sizeof instances);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top_inputs{};
    top_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top_inputs.NumDescs = 2;
    top_inputs.InstanceDescs = instance_buffer->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO top_sizes{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&top_inputs, &top_sizes);
    auto top = buffer(device.Get(), top_sizes.ResultDataMaxSizeInBytes,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto top_scratch = buffer(device.Get(), top_sizes.ScratchDataSizeInBytes,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = bottom_inputs;
    build.DestAccelerationStructureData = bottom->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = bottom_scratch->GetGPUVirtualAddress();
    commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = bottom.Get();
    commands->ResourceBarrier(1, &barrier);
    build.Inputs = top_inputs;
    build.DestAccelerationStructureData = top->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = top_scratch->GetGPUVirtualAddress();
    commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    barrier.UAV.pResource = top.Get();
    commands->ResourceBarrier(1, &barrier);

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 2;
    root_desc.pParameters = parameters;
    ComPtr<ID3DBlob> root_blob, errors;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &root_blob, &errors), "serialize root signature");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, root_blob->GetBufferPointer(),
        root_blob->GetBufferSize(), IID_PPV_ARGS(&root)), "create root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {kDxrSmokeShader, sizeof kDxrSmokeShader};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&pipeline_desc,
        IID_PPV_ARGS(&pipeline)), "create ray query pipeline");

    struct Result { uint32_t hit, instance, primitive; float distance; };
    static_assert(sizeof(Result) == 16, "HLSL result stride");
    const Result expected[] = {
        {1, 7, 0, 1}, {1, 11, 0, 3},
        {0, UINT32_MAX, UINT32_MAX, -1}, {0, UINT32_MAX, UINT32_MAX, -1}
    };
    auto output = buffer(device.Get(), sizeof expected, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = buffer(device.Get(), sizeof expected, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST);
    commands->SetComputeRootSignature(root.Get());
    commands->SetPipelineState(pipeline.Get());
    commands->SetComputeRootShaderResourceView(0, top->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    commands->Dispatch(1, 1, 1);
    barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commands->ResourceBarrier(1, &barrier);
    commands->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, sizeof expected);
    check(commands->Close(), "close command list");

    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create fence");
    ID3D12CommandList *lists[] = {commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), 1), "signal fence");
    const ULONGLONG started = GetTickCount64();
    while (fence->GetCompletedValue() < 1 && GetTickCount64() - started < 10000) Sleep(1);
    check(device->GetDeviceRemovedReason(), "GPU execution");
    if (fence->GetCompletedValue() != 1) throw std::runtime_error("GPU timed out after 10 seconds");

    void *mapped = nullptr;
    const D3D12_RANGE read_range{0, sizeof expected}, no_write{0, 0};
    check(readback->Map(0, &read_range, &mapped), "map results");
    Result actual[4];
    std::memcpy(actual, mapped, sizeof actual);
    readback->Unmap(0, &no_write);
    bool passed = true;
    for (unsigned i = 0; i < 4; ++i) {
        const bool match = actual[i].hit == expected[i].hit &&
            actual[i].instance == expected[i].instance &&
            actual[i].primitive == expected[i].primitive &&
            std::isfinite(actual[i].distance) &&
            std::fabs(actual[i].distance - expected[i].distance) < 0.0001f;
        std::printf("%s ray=%u hit=%u instance=%u primitive=%u distance=%.4f\n",
            match ? "PASS" : "FAIL", i, actual[i].hit, actual[i].instance,
            actual[i].primitive, actual[i].distance);
        passed = passed && match;
    }
    passed = runInterop(device.Get(), selected_adapter.Get(), queue.Get(),
        top->GetGPUVirtualAddress()) && passed;
    if (messages) {
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            check(messages->GetMessage(i, nullptr, &size), "size debug message");
            std::vector<unsigned char> storage(size);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            check(messages->GetMessage(i, message, &size), "read debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
                std::fprintf(stderr, "D3D12: %s\n", message->pDescription);
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) passed = false;
            }
        }
    }
    std::puts(passed ? "PASS hardware DXR smoke test" : "FAIL hardware DXR smoke test");
    return passed ? 0 : 1;
}

int main(int argc, char **argv)
{
    try {
        if (argc == 2 && (std::strcmp(argv[1], "--dx11-only") == 0 ||
            std::strcmp(argv[1], "--dx11-warp") == 0)) {
            return runInterop(nullptr, nullptr, nullptr, 0,
                std::strcmp(argv[1], "--dx11-warp") == 0) ? 0 : 1;
        }
        if (argc != 1) {
            std::fprintf(stderr, "Usage: recomp_dxr_smoke_test [--dx11-only|--dx11-warp]\n");
            return 1;
        }
        return run();
    } catch (HRESULT) {
        return 1;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
