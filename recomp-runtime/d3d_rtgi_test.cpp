#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "d3d_rtgi.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static void check(HRESULT result, const char *operation)
{
    if (FAILED(result)) {
        std::fprintf(stderr, "FAIL %s: hr=0x%08lX\n", operation, static_cast<unsigned long>(result));
        throw result;
    }
}

static void require(bool passed, const char *message)
{
    if (!passed) throw std::runtime_error(message);
}

using Rtgi = std::unique_ptr<D3dRtgi, decltype(&d3d_rtgi_destroy)>;
constexpr UINT width = 321, height = 241;
// The last two floats also allow XYZ/normal/UV fixtures; raster uses position/weight only.
struct Vertex { float x, y, z, weight, u, v, extraU, extraV; };
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    RecompD3dPresenterDrawCommand draw{};
    ComPtr<ID3D11ShaderResourceView> texture;
    float color[4];
    bool colorMaterial = false;
};

static XMFLOAT4X4 camera(float shift = 0)
{
    XMFLOAT4X4 result;
    XMStoreFloat4x4(&result, XMMatrixMultiply(
        XMMatrixLookAtLH(XMVectorSet(9 + shift, 8, -13, 1), XMVectorSet(3 + shift, 0, 0, 1), XMVectorSet(0, 1, 0, 0)),
        XMMatrixOrthographicLH(21, 21 * float(height) / width, 0.1f, 60)));
    return result;
}

static std::array<Mesh, 3> scene(bool baked)
{
    std::array<Mesh, 3> meshes{};
    meshes[0].vertices = {{0, 0, -5, 0.25f}, {12, 0, -5, 0.25f},
        {12, 0, 5, 0.25f}, {0, 0, 5, 0.25f}};
    meshes[0].indices = baked ? std::vector<uint16_t>{0, 1, 2, 0, 2, 3} : std::vector<uint16_t>{0, 1, 2, 3};
    meshes[0].draw.primitive_type = baked ? 5 : 7;
    // Positive bounce from the floor must leave the saturated wall unchanged.
    meshes[0].color[0] = meshes[0].color[1] = meshes[0].color[2] = 0.65f;
    meshes[1].vertices = {{0, 0, -5, 0}, {0, 3, -5, 0}, {0, 0, 0, 0}, {0, 3, 0, 0}};
    meshes[1].indices = baked ? std::vector<uint16_t>{0, 1, 2, 2, 1, 3} : std::vector<uint16_t>{0, 1, 2, 3};
    meshes[1].draw.primitive_type = baked ? 5 : 6;
    meshes[1].color[0] = 1;
    const float x = baked ? 0.0f : -4.0f;
    meshes[2].vertices = {{x, 0, 0, 0.5f}, {x, 3, 0, 0.5f}, {x, 0, 5, 0.5f}, {x, 3, 5, 0.5f}};
    meshes[2].indices = {0, 1, 2, 2, 1, 3};
    meshes[2].draw.primitive_type = 5;
    meshes[2].draw.blend_weight_count = baked ? 0 : 1;
    meshes[2].color[0] = 1;
    const auto viewProjection = camera();
    for (auto &mesh : meshes) {
        mesh.color[3] = 1;
        auto &draw = mesh.draw;
        draw.has_transform = true;
        draw.depth.depth_write_enable = true;
        draw.triangle_count = 2;
        draw.vertex_count = 4;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x006; // Carries a weight even when skinning is disabled.
        for (auto &world : draw.world) world[0] = world[5] = world[10] = world[15] = 1;
        draw.world[1][12] = 8;
        std::memcpy(draw.view_projection, &viewProjection, sizeof viewProjection);
    }
    return meshes;
}

static const char sceneSource[] = R"(
cbuffer Draw : register(b0) {
    row_major float4x4 world[2];
    row_major float4x4 camera;
    float4 color;
    uint blendCount; float3 padding;
};
float4 vs(float3 p : POSITION, float weight : BLENDWEIGHT) : SV_Position {
    float4 position = mul(float4(p, 1), world[0]);
    if (blendCount != 0)
        position = weight * position + (1 - weight) * mul(float4(p, 1), world[1]);
    return mul(position, camera);
}
float4 ps() : SV_Target { return color; }
)";

struct Renderer {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ComPtr<ID3D11Texture2D> color, resolved, depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> colorSRV, depthSRV;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11Buffer> constants;
    UINT samples, width, height;

    Renderer(ID3D11Device *d, ID3D11DeviceContext *c, UINT s, UINT w = ::width, UINT h = ::height)
        : device(d), context(c), samples(s), width(w), height(h)
    {
        UINT quality = 0;
        check(device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, samples, &quality), "query color MSAA");
        require(quality != 0, "requested color MSAA unsupported");
        check(device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, samples, &quality), "query depth MSAA");
        require(quality != 0, "requested depth MSAA unsupported");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc.Count = samples;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&desc, nullptr, &color), "create scene color");
        check(device->CreateRenderTargetView(color.Get(), nullptr, &rtv), "create scene RTV");
        desc.SampleDesc.Count = 1;
        check(device->CreateTexture2D(&desc, nullptr, &resolved), "create resolved color");
        check(device->CreateShaderResourceView(resolved.Get(), nullptr, &colorSRV), "create color SRV");
        desc.SampleDesc.Count = samples;
        desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&desc, nullptr, &depth), "create typeless depth");
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        check(device->CreateDepthStencilView(depth.Get(), &dv, &dsv), "create scene DSV");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sv.ViewDimension = samples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        if (samples == 1) sv.Texture2D.MipLevels = 1;
        check(device->CreateShaderResourceView(depth.Get(), &sv, &depthSRV), "create scene depth SRV");
        ComPtr<ID3DBlob> vertex, pixel, errors;
        check(D3DCompile(sceneSource, sizeof sceneSource - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &vertex, &errors), "compile scene VS");
        check(D3DCompile(sceneSource, sizeof sceneSource - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &pixel, &errors), "compile scene PS");
        check(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs), "create scene VS");
        check(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &ps), "create scene PS");
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"BLENDWEIGHT", 0, DXGI_FORMAT_R32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        check(device->CreateInputLayout(elements, 2, vertex->GetBufferPointer(), vertex->GetBufferSize(), &layout), "create scene layout");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = true;
        raster.MultisampleEnable = true;
        check(device->CreateRasterizerState(&raster, &rasterizer), "create scene rasterizer");
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = 224;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&cb, nullptr, &constants), "create scene constants");
    }

    void resolve()
    {
        context->ClearState();
        if (samples > 1) context->ResolveSubresource(resolved.Get(), 0, color.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
        else context->CopyResource(resolved.Get(), color.Get());
    }

    template <class Meshes> void render(Meshes &meshes)
    {
        context->ClearState();
        const float sky[] = {0.07f, 0.12f, 0.2f, 1};
        context->ClearRenderTargetView(rtv.Get(), sky);
        context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer.Get());
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        for (auto &mesh : meshes) {
            if (mesh.indices.empty()) continue;
            std::vector<uint16_t> indices = mesh.indices;
            if (mesh.draw.primitive_type == 6) indices = {indices[0], indices[1], indices[2], indices[2], indices[1], indices[3]};
            if (mesh.draw.primitive_type == 7) indices = {indices[0], indices[1], indices[2], indices[0], indices[2], indices[3]};
            D3D11_BUFFER_DESC desc{};
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.ByteWidth = UINT(mesh.vertices.size() * sizeof(Vertex));
            desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA data{mesh.vertices.data(), 0, 0};
            ComPtr<ID3D11Buffer> vb, ib;
            check(device->CreateBuffer(&desc, &data, &vb), "create scene vertices");
            desc.ByteWidth = UINT(indices.size() * sizeof(uint16_t));
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            data.pSysMem = indices.data();
            check(device->CreateBuffer(&desc, &data, &ib), "create scene indices");
            const UINT stride = sizeof(Vertex), offset = 0;
            context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
            context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
            struct Constants { float world[2][16], camera[16], color[4]; UINT blend, padding[3]; } values{};
            static_assert(sizeof values == 224, "scene HLSL constants");
            std::memcpy(values.world, mesh.draw.world, sizeof values.world);
            std::memcpy(values.camera, mesh.draw.view_projection, sizeof values.camera);
            std::memcpy(values.color, mesh.color, sizeof values.color);
            values.blend = mesh.draw.blend_weight_count;
            context->UpdateSubresource(constants.Get(), 0, nullptr, &values, 0, 0);
            context->DrawIndexed(UINT(indices.size()), 0, 0);
        }
        resolve();
    }
};

using Pixels = std::vector<std::array<uint8_t, 4>>;

static void waitReadback(Renderer &r)
{
    ComPtr<ID3D11Query> query;
    const D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
    check(r.device->CreateQuery(&queryDesc, &query), "create completion query");
    r.context->End(query.Get());
    r.context->Flush();
    const ULONGLONG start = GetTickCount64();
    HRESULT ready;
    while ((ready = r.context->GetData(query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE &&
        GetTickCount64() - start < 10000) Sleep(1);
    check(ready, "readback completion");
    require(ready == S_OK, "readback timed out after 10 s");
}

static Pixels readback(Renderer &r, ID3D11ShaderResourceView *view)
{
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(r.device->CreateTexture2D(&desc, nullptr, &staging), "create readback");
    r.context->CopyResource(staging.Get(), resource.Get());
    waitReadback(r);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(r.context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped), "map readback");
    Pixels pixels(width * height);
    for (UINT y = 0; y < height; ++y)
        std::memcpy(pixels.data() + y * width, static_cast<const char *>(mapped.pData) + y * mapped.RowPitch, width * 4);
    r.context->Unmap(staging.Get(), 0);
    return pixels;
}

static Pixels readSamples(Renderer &r)
{
    static const char source[] = R"(
Texture2DMS<float4> image : register(t0);
RWStructuredBuffer<uint> output : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 p : SV_DispatchThreadID) {
    uint width, height, count;
    image.GetDimensions(width, height, count);
    if (p.x >= width || p.y >= height) return;
    for (uint s = 0; s < count; ++s) {
        uint4 rgba = uint4(round(image.Load(p.xy, s) * 255));
        output[(p.y * width + p.x) * count + s] =
            rgba.r | (rgba.g << 8) | (rgba.b << 16) | (rgba.a << 24);
    }
})";
    ComPtr<ID3DBlob> blob, errors;
    check(D3DCompile(source, sizeof source - 1, nullptr, nullptr, nullptr, "main", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &blob, &errors), "compile sample reader");
    ComPtr<ID3D11ComputeShader> shader;
    check(r.device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader), "create sample reader");
    ComPtr<ID3D11ShaderResourceView> input;
    check(r.device->CreateShaderResourceView(r.color.Get(), nullptr, &input), "create MSAA read SRV");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = width * height * r.samples * 4;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = 4;
    ComPtr<ID3D11Buffer> output, staging;
    check(r.device->CreateBuffer(&desc, nullptr, &output), "create sample output");
    ComPtr<ID3D11UnorderedAccessView> uav;
    check(r.device->CreateUnorderedAccessView(output.Get(), nullptr, &uav), "create sample output UAV");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(r.device->CreateBuffer(&desc, nullptr, &staging), "create sample staging");
    r.context->ClearState();
    r.context->CSSetShader(shader.Get(), nullptr, 0);
    r.context->CSSetShaderResources(0, 1, input.GetAddressOf());
    r.context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    r.context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    r.context->ClearState();
    r.context->CopyResource(staging.Get(), output.Get());
    waitReadback(r);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(r.context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped), "map samples");
    Pixels pixels(width * height * r.samples);
    std::memcpy(pixels.data(), mapped.pData, desc.ByteWidth);
    r.context->Unmap(staging.Get(), 0);
    return pixels;
}

static void addScene(D3dRtgi *gi, std::array<Mesh, 3> &meshes, bool extras)
{
    for (auto &mesh : meshes) {
        mesh.draw.vertex_bytes = mesh.vertices.data();
        mesh.draw.index_bytes = mesh.indices.data();
        mesh.draw.index_count = UINT(mesh.indices.size());
        require(d3d_rtgi_add_draw(gi, mesh.draw, mesh.texture.Get(), nullptr, mesh.colorMaterial) == !mesh.indices.empty(), "add_draw acceptance mismatch");
    }
    if (!extras) return;
    // All these must be ignored, including their deliberately different camera.
    auto draw = meshes[2].draw;
    draw.world[0][12] = 4;
    draw.world[1][12] = 4;
    draw.view_projection[12] += 100;
    draw.target.offscreen = true;
    require(!d3d_rtgi_add_draw(gi, draw, nullptr, nullptr), "accepted offscreen draw");
    draw.target.offscreen = false;
    draw.depth.depth_write_enable = false;
    require(!d3d_rtgi_add_draw(gi, draw, nullptr, nullptr), "accepted non-depth-writing draw");
    draw.depth.depth_write_enable = true;
    draw.has_transform = false;
    require(!d3d_rtgi_add_draw(gi, draw, nullptr, nullptr), "accepted untransformed draw");
    draw.has_transform = true;
    draw.fvf = 0x004;
    require(!d3d_rtgi_add_draw(gi, draw, nullptr, nullptr), "accepted pretransformed draw");
    draw.fvf = 0x006;
    draw.program_count = 1;
    require(!d3d_rtgi_add_draw(gi, draw, nullptr, nullptr), "accepted vertex-program draw");
    draw.program_count = 0;
    draw.primitive_type = 4;
    require(!d3d_rtgi_add_draw(gi, draw, nullptr, nullptr), "accepted non-triangle draw");
    draw.primitive_type = 5;
    const uint16_t invalid[] = {0, 1, 65000, 0, 0, 1};
    draw.index_bytes = invalid;
    require(!d3d_rtgi_add_draw(gi, draw, nullptr, nullptr), "accepted invalid triangles");
}

static void apply(D3dRtgi *gi, Renderer &r)
{
    d3d_rtgi_apply(gi, r.context, r.rtv.Get(), r.colorSRV.Get(), r.depthSRV.Get(), r.samples);
    ComPtr<ID3D11VertexShader> vs;
    r.context->VSGetShader(&vs, nullptr, nullptr);
    require(!vs, "apply did not clear D3D11 context state");
}

static Pixels readTarget(Renderer &r)
{
    r.resolve();
    return readback(r, r.colorSRV.Get());
}

static Pixels runFrames(Renderer &renderer, std::array<Mesh, 3> &meshes, bool extras, UINT frames = 48)
{
    Rtgi gi(d3d_rtgi_create(renderer.device, width, height), d3d_rtgi_destroy);
    require(bool(gi), "RTGI creation failed on a supported adapter");
    renderer.render(meshes);
    const auto input = readTarget(renderer);
    apply(gi.get(), renderer);
    require(readTarget(renderer) == input, "empty scene changed target");
    for (UINT frame = 0; frame < frames; ++frame) {
        renderer.render(meshes);
        addScene(gi.get(), meshes, extras);
        apply(gi.get(), renderer);
    }
    require(readback(renderer, renderer.colorSRV.Get()) == input, "module changed resolved input color");
    auto pixels = readTarget(renderer);
    apply(gi.get(), renderer);
    require(readTarget(renderer) == pixels, "apply retained previous draws");
    return pixels;
}

static size_t pixelAt(float x, float y, float z, const XMFLOAT4X4 &matrix)
{
    const auto clip = XMVector3TransformCoord(XMVectorSet(x, y, z, 1), XMLoadFloat4x4(&matrix));
    const int px = int((XMVectorGetX(clip) * 0.5f + 0.5f) * width);
    const int py = int((-XMVectorGetY(clip) * 0.5f + 0.5f) * height);
    require(px >= 0 && px < int(width) && py >= 0 && py < int(height), "sample outside scene");
    return size_t(py) * width + px;
}

static bool verifyImage(const Pixels &input, const Pixels &output, UINT samples, const XMFLOAT4X4 &matrix)
{
    double nearGain[3]{}, farGain[3]{};
    unsigned floorCount = 0;
    for (unsigned z = 0; z < 12; ++z) for (unsigned x = 0; x < 8; ++x) {
        const float depth = -2.5f + z * 0.45f;
        const size_t n = pixelAt(0.6f + 0.12f * x, 0, depth, matrix);
        const size_t f = pixelAt(9.0f + 0.12f * x, 0, depth, matrix);
        require(input[n][2] == input[n][1] && input[f][2] == input[f][1], "floor samples not on white floor");
        for (unsigned c = 0; c < 3; ++c) {
            nearGain[c] += int(output[n][c]) - input[n][c];
            farGain[c] += int(output[f][c]) - input[f][c];
        }
        ++floorCount;
    }
    for (unsigned c = 0; c < 3; ++c) { nearGain[c] /= floorCount; farGain[c] /= floorCount; }
    const bool bounce = nearGain[2] > 4 && nearGain[2] > nearGain[1] + 3 && nearGain[2] > nearGain[0] + 3;
    const bool falloff = farGain[2] < nearGain[2] * 0.35 + 1;
    unsigned skyChanged = 0, skyCount = 0, wallError = 0;
    for (size_t p = 0; p < input.size(); ++p) {
        if (input[p] == input[0]) { ++skyCount; skyChanged += input[p] != output[p]; }
    }
    for (unsigned z = 0; z < 12; ++z) for (unsigned y = 0; y < 6; ++y) {
        const size_t p = pixelAt(0, 0.7f + y * 0.3f, -2.5f + z * 0.45f, matrix);
        require(input[p][2] == 255 && input[p][1] == 0, "wall sample not on red wall");
        for (unsigned c = 0; c < 4; ++c) wallError = std::max(wallError, unsigned(std::abs(int(output[p][c]) - input[p][c])));
    }
    std::printf("%s RTGI %ux MSAA red bounce: near RGB=(%.2f,%.2f,%.2f) far RGB=(%.2f,%.2f,%.2f)\n",
        bounce && falloff ? "PASS" : "FAIL", samples, nearGain[2], nearGain[1], nearGain[0], farGain[2], farGain[1], farGain[0]);
    const bool stable = skyCount > 100 && skyChanged == 0 && wallError <= 1;
    std::printf("%s RTGI %ux MSAA sky unchanged=%u/%u wall max error=%u\n",
        stable ? "PASS" : "FAIL", samples, skyCount - skyChanged, skyCount, wallError);
    return bounce && falloff && stable;
}

static bool verifyHistory(Renderer &renderer)
{
    Rtgi gi(d3d_rtgi_create(renderer.device, width, height), d3d_rtgi_destroy);
    require(bool(gi), "create history test module");
    auto meshes = scene(false);
    // Move the camera during accumulation, then remove the lit floor entirely.
    for (unsigned frame = 0; frame < 48; ++frame) {
        const auto matrix = camera(frame < 24 ? 0 : 0.6f);
        for (auto &mesh : meshes) std::memcpy(mesh.draw.view_projection, &matrix, sizeof matrix);
        renderer.render(meshes);
        addScene(gi.get(), meshes, false);
        apply(gi.get(), renderer);
    }
    auto input = readback(renderer, renderer.colorSRV.Get());
    const auto moving = readTarget(renderer);
    const size_t nearPixel = pixelAt(1, 0, 1, camera(0.6f));
    // The red wall brightens red and occludes green/blue after reprojection.
    bool passed = int(moving[nearPixel][2]) > input[nearPixel][2] + 4 &&
        int(moving[nearPixel][1]) < input[nearPixel][1] - 4 &&
        int(moving[nearPixel][0]) < input[nearPixel][0] - 4;
    meshes[0].indices.clear();
    renderer.render(meshes);
    addScene(gi.get(), meshes, false);
    apply(gi.get(), renderer);
    input = readback(renderer, renderer.colorSRV.Get());
    const auto removed = readTarget(renderer);
    // Previously lit floor is sky now: no stale history may survive there.
    for (size_t p = 0; p < input.size(); ++p)
        if (input[p] == input[0]) passed = passed && input[p] == removed[p];
    std::printf("%s RTGI %ux MSAA camera reprojection / disoccluded sky\n", passed ? "PASS" : "FAIL", renderer.samples);
    return passed;
}

static bool verifySamples(Renderer &renderer)
{
    auto meshes = scene(false);
    meshes[0].color[3] = 0.37f;
    renderer.render(meshes);
    const auto before = readSamples(renderer);
    unsigned edges = 0;
    for (size_t p = 0; p < before.size(); p += renderer.samples)
        for (UINT s = 1; s < renderer.samples; ++s) edges += before[p] != before[p + s];
    require(edges > 100, "sample preservation fixture has no hard MSAA edges");
    Rtgi gi(d3d_rtgi_create(renderer.device, width, height), d3d_rtgi_destroy);
    require(bool(gi), "create sample preservation module");
    addScene(gi.get(), meshes, false);
    apply(gi.get(), renderer);
    const bool passed = readSamples(renderer) == before;
    apply(gi.get(), renderer);
    require(readSamples(renderer) == before, "empty scene changed MSAA samples");
    std::printf("%s RTGI 4x MSAA per-sample zero strength: %zu samples, %u edge differences\n",
        passed ? "PASS" : "FAIL", before.size(), edges);
    return passed;
}

static bool verifyWeakLighting(Renderer &renderer)
{
    // Every undithered change is below half an output byte and would vanish.
    require(_putenv_s("RECOMP_D3D_RTGI_STRENGTH", "0.004") == 0, "set weak lighting");
    auto meshes = scene(true);
    renderer.render(meshes);
    const auto input = readTarget(renderer);
    const auto output = runFrames(renderer, meshes, false);
    unsigned changed = 0, error = 0;
    for (size_t p = 0; p < input.size(); ++p) {
        require(input[p][3] == output[p][3], "dither changed alpha");
        if (input[p] != std::array<uint8_t, 4>{166, 166, 166, 255}) continue;
        changed += input[p] != output[p];
        for (unsigned c = 0; c < 3; ++c)
            error = std::max(error, unsigned(std::abs(int(input[p][c]) - output[p][c])));
    }
    require(_putenv_s("RECOMP_D3D_RTGI_STRENGTH", "1.5") == 0, "restore strength");
    const bool passed = changed > 100 && error == 1;
    std::printf("%s RTGI %ux MSAA sub-byte lighting: changed=%u max byte error=%u\n",
        passed ? "PASS" : "FAIL", renderer.samples, changed, error);
    return passed;
}

static bool verifyBanding(Renderer &renderer)
{
    // Red radiance is now 1 - 0.5; double strength to retain the reference signal.
    require(_putenv_s("RECOMP_D3D_RTGI_STRENGTH", "3") == 0, "set banding strength");
    auto meshes = scene(true);
    meshes[0].color[0] = meshes[0].color[1] = meshes[0].color[2] = 0.35f;
    // A shallow perspective view with a tiny near plane makes D24 steps large
    // compared with the floor's projected slope. The wall spans the full width
    // with no material or geometry boundaries inside the measured region.
    // Scale scene and frustum together: retain D24 stress within skyDistance.
    meshes[0].vertices = {{-1000, 0, 10, 0}, {1000, 0, 10, 0},
        {1000, 0, 95, 0}, {-1000, 0, 95, 0}};
    meshes[1].vertices = {{-1000, 0, 95, 0}, {-1000, 100, 95, 0},
        {1000, 0, 95, 0}, {1000, 100, 95, 0}};
    meshes[2].indices.clear();
    XMFLOAT4X4 matrix;
    const auto view = XMMatrixLookAtLH(XMVectorSet(0, 2, 0, 1),
        XMVectorSet(0, 0, 100, 1), XMVectorSet(0, 1, 0, 0));
    XMStoreFloat4x4(&matrix, view *
        XMMatrixPerspectiveFovLH(0.16f, float(width) / height, 0.00002f, 2000));
    for (auto &mesh : meshes) std::memcpy(mesh.draw.view_projection, &matrix, sizeof matrix);
    renderer.render(meshes);
    const auto input = readTarget(renderer);
    const auto output = runFrames(renderer, meshes, false, 96);
    XMStoreFloat4x4(&matrix, view * XMMatrixPerspectiveFovLH(0.16f, float(width) / height, 0.1f, 2000));
    for (auto &mesh : meshes) std::memcpy(mesh.draw.view_projection, &matrix, sizeof matrix);
    renderer.render(meshes);
    require(readTarget(renderer) == input, "near-plane reference changed raster color");
    const auto reference = runFrames(renderer, meshes, false, 96);
    require(_putenv_s("RECOMP_D3D_RTGI_STRENGTH", "1.5") == 0, "restore strength");
    double rowError = 0, stepError = 0, previousError = 0, gain = 0;
    unsigned rows = 0;
    // Stay inside the lit floor, excluding the wall seam and raster edges.
    for (UINT y = 125; y < 145; ++y) {
        double error = 0;
        for (UINT x = 48; x < width - 48; ++x) {
            const size_t p = size_t(y) * width + x;
            require(input[p][2] == input[p][1] && input[p][2] == 89, "banding sample outside floor");
            error += int(output[p][2]) - reference[p][2];
            gain += int(reference[p][2]) - input[p][2];
        }
        error /= width - 96;
        rowError += std::abs(error);
        if (rows) stepError += std::abs(error - previousError);
        previousError = error;
        ++rows;
    }
    rowError /= rows;
    stepError /= rows - 1;
    gain /= rows * (width - 96);
    // Row means suppress Monte Carlo noise. Subtract the well-conditioned
    // reference before measuring row-to-row steps to remove real light falloff.
    // Errors are in 8-bit red-channel levels; both paths use identical ray seeds.
    const bool passed = gain > 1 && rowError < 1 && stepError < 0.5;
    std::printf("%s RTGI %ux MSAA grazing-floor banding: row error=%.4f step error=%.4f reference gain=%.2f (limits 1/0.5)\n",
        passed ? "PASS" : "FAIL", renderer.samples, rowError, stepError, gain);
    return passed;
}

// A faceted, skinned sphere exercises normal changes and camera-only reprojection.
static std::array<Mesh, 3> movingScene()
{
    auto meshes = scene(true);
    meshes[0].color[0] = meshes[0].color[1] = meshes[0].color[2] = 0.7f;
    meshes[1].vertices = {{0, 0, -5, 0}, {0, 6, -5, 0}, {0, 0, 5, 0}, {0, 6, 5, 0}};
    meshes[1].color[1] = 0.18f;
    meshes[1].color[2] = 0.03f;
    auto &sphere = meshes[2];
    sphere.vertices.clear();
    sphere.indices.clear();
    constexpr unsigned rings = 12, segments = 24;
    for (unsigned y = 0; y <= rings; ++y) for (unsigned x = 0; x <= segments; ++x) {
        const float latitude = XM_PI * y / rings, longitude = XM_2PI * x / segments;
        sphere.vertices.push_back({1.3f * std::sin(latitude) * std::cos(longitude),
            1.3f * std::cos(latitude), 1.3f * std::sin(latitude) * std::sin(longitude),
            0.5f + 0.4f * std::cos(latitude)});
    }
    for (unsigned y = 0; y < rings; ++y) for (unsigned x = 0; x < segments; ++x) {
        const uint16_t a = uint16_t(y * (segments + 1) + x), b = a + segments + 1;
        if (y) sphere.indices.insert(sphere.indices.end(), {a, b, uint16_t(a + 1)});
        if (y + 1 < rings) sphere.indices.insert(sphere.indices.end(), {uint16_t(a + 1), b, uint16_t(b + 1)});
    }
    sphere.draw.vertex_count = UINT(sphere.vertices.size());
    sphere.draw.triangle_count = UINT(sphere.indices.size() / 3);
    sphere.draw.blend_weight_count = 1;
    sphere.color[0] = 0.32f; sphere.color[1] = 0.24f; sphere.color[2] = 0.18f;
    XMFLOAT4X4 matrix;
    XMStoreFloat4x4(&matrix, XMMatrixLookAtLH(XMVectorSet(6, 4, -8, 1),
        XMVectorSet(2, 1, 0, 1), XMVectorSet(0, 1, 0, 0)) *
        XMMatrixOrthographicLH(8, 8 * float(height) / width, 0.1f, 40));
    for (auto &mesh : meshes) std::memcpy(mesh.draw.view_projection, &matrix, sizeof matrix);
    return meshes;
}

static void moveSphere(Mesh &sphere, unsigned frame)
{
    for (unsigned bone = 0; bone < 2; ++bone) {
        XMFLOAT4X4 world;
        XMStoreFloat4x4(&world, XMMatrixRotationY(0.07f * frame + 0.12f * bone) *
            XMMatrixTranslation(2 + 0.35f * std::sin(frame * 0.3f + bone * 0.2f),
                1.5f + 0.06f * std::sin(frame * 0.4f + bone), 0));
        std::memcpy(sphere.draw.world[bone], &world, sizeof world);
    }
}

static double contribution(const std::array<uint8_t, 4> &input, const std::array<uint8_t, 4> &output)
{
    double sum = 0;
    for (unsigned c = 0; c < 3; ++c) {
        require(input[c] >= 16, "contribution input too dark");
        require(output[c] < 250, "contribution clipped by UNORM target");
        sum += double(int(output[c]) - input[c]) / input[c];
    }
    return sum / 3;
}

static bool verifyMovingNoise(Renderer &r)
{
    auto meshes = movingScene();
    // White surroundings supply a positive signal without opposing RGB changes.
    meshes[0].color[0] = meshes[0].color[1] = meshes[0].color[2] = 1;
    meshes[1].color[1] = meshes[1].color[2] = 1;
    require(meshes[2].draw.triangle_count >= 200, "rounded fixture too small");
    Rtgi gi(d3d_rtgi_create(r.device, width, height), d3d_rtgi_destroy);
    require(bool(gi), "create moving noise module");
    // -2 is outside the signed composite range, including complete occlusion.
    std::vector<double> previous(width * height, -2);
    double spatial = 0, temporal = 0, gain = 0, temporalGain = 0;
    unsigned count = 0, temporalCount = 0;
    for (unsigned frame = 0; frame < 32; ++frame) {
        moveSphere(meshes[2], frame);
        r.render(meshes);
        const auto input = readTarget(r);
        addScene(gi.get(), meshes, false);
        apply(gi.get(), r);
        const auto output = readTarget(r);
        std::vector<double> current(input.size(), -2);
        for (size_t p = 0; p < input.size(); ++p)
            if (input[p] == std::array<uint8_t, 4>{46, 61, 82, 255})
                current[p] = contribution(input[p], output[p]);
        // Measure lighting over a complete dither tile. verifyWeakLighting bounds
        // the intentional per-pixel grain to one byte and checks alpha separately.
        auto spatialSignal = current;
        for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
            double mean = 0;
            bool interior = x + 3 < width && y + 3 < height;
            if (interior) for (UINT dy = 0; dy < 4; ++dy) for (UINT dx = 0; dx < 4; ++dx) {
                const double value = current[size_t(y + dy) * width + x + dx];
                interior &= value != -2;
                mean += value / 16;
            }
            spatialSignal[size_t(y) * width + x] = interior ? mean : -2;
        }
        if (frame >= 24) for (UINT y = 1; y + 1 < height; ++y) for (UINT x = 1; x + 1 < width; ++x) {
            const size_t p = size_t(y) * width + x;
            double mean = 0;
            bool interior = true;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const double value = spatialSignal[size_t(int(y) + dy) * width + int(x) + dx];
                interior &= value != -2;
                mean += value / 9;
            }
            if (!interior) continue;
            spatial += std::abs(spatialSignal[p] - mean);
            gain += std::abs(spatialSignal[p]);
            ++count;
            if (previous[p] != -2) {
                temporal += std::abs(current[p] - previous[p]);
                temporalGain += (std::abs(current[p]) + std::abs(previous[p])) * 0.5;
                ++temporalCount;
            }
        }
        previous = std::move(current);
    }
    require(count > 1000 && temporalCount > 1000 && gain / count > 0.025,
        "moving noise fixture has no measurable change");
    spatial /= gain;
    temporal /= temporalGain;
    const bool passed = spatial < 0.02 && temporal < 0.09;
    std::printf("%s RTGI %ux MSAA moving sphere: spatial=%.6f temporal=%.6f mean absolute change=%.6f pixels=%u (limits .02/.09)\n",
        passed ? "PASS" : "FAIL", r.samples, spatial, temporal, gain / count, count);
    return passed;
}

static bool verifyFireflies(Renderer &r)
{
    auto meshes = movingScene();
    meshes[0].color[0] = meshes[0].color[1] = meshes[0].color[2] = 0.25f;
    // Neutral sky-level surroundings leave the small white patch as the source.
    meshes[1].color[0] = meshes[1].color[1] = meshes[1].color[2] = 0.5f;
    auto &patch = meshes[2];
    // Keep a rare source, but large enough for the halved radiance to exceed 1 LSB.
    patch.vertices = {{0.002f, 0.3f, -0.4f, 0}, {0.002f, 1.1f, -0.4f, 0},
        {0.002f, 0.3f, 0.4f, 0}, {0.002f, 1.1f, 0.4f, 0}};
    patch.indices = {0, 1, 2, 2, 1, 3};
    patch.draw.vertex_count = 4;
    patch.draw.triangle_count = 2;
    patch.draw.blend_weight_count = 0;
    patch.color[0] = patch.color[1] = patch.color[2] = 1;
    Rtgi gi(d3d_rtgi_create(r.device, width, height), d3d_rtgi_destroy);
    require(bool(gi), "create firefly module");
    unsigned outliers = 0;
    double peak = 0, total = 0;
    for (unsigned frame = 0; frame < 24; ++frame) {
        r.render(meshes);
        const auto input = readTarget(r);
        addScene(gi.get(), meshes, false);
        apply(gi.get(), r);
        const auto output = readTarget(r);
        std::vector<double> values(input.size(), -2);
        for (size_t p = 0; p < input.size(); ++p)
            if (input[p] == std::array<uint8_t, 4>{64, 64, 64, 255}) {
                values[p] = contribution(input[p], output[p]);
                total += std::max(0.0, values[p]);
                peak = std::max(peak, values[p]);
            }
        for (UINT y = 1; y + 1 < height; ++y) for (UINT x = 1; x + 1 < width; ++x) {
            const size_t p = size_t(y) * width + x;
            double maximum = 0;
            bool interior = true;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const double value = values[size_t(int(y) + dy) * width + int(x) + dx];
                interior &= value != -2;
                maximum = std::max(maximum, value);
            }
            // A one-LSB rounding step can double a 1-LSB gain; require more than that.
            outliers += interior && values[p] != -2 && values[p] > 0.03 &&
                values[p] > 1.5 * maximum && values[p] - maximum > 1.0 / 64;
        }
    }
    const bool passed = total > 1 && peak > 0.03 && outliers <= 2;
    std::printf("%s RTGI %ux MSAA fireflies: isolated=%u peak=%.6f total=%.6f (limit 2 over 24 frames)\n",
        passed ? "PASS" : "FAIL", r.samples, outliers, peak, total);
    return passed;
}

static bool verifyOcclusion(Renderer &r)
{
    auto meshes = scene(true);
    meshes[0].color[0] = meshes[0].color[1] = meshes[0].color[2] = 0.35f;
    // The camera sees the white top; floor rays hit its hidden underside
    // within the 0.5-unit contact falloff.
    meshes[1].vertices = {{1, 0.2f, -2, 0}, {4, 0.2f, -2, 0},
        {1, 0.2f, 2, 0}, {4, 0.2f, 2, 0}};
    meshes[1].color[0] = meshes[1].color[1] = meshes[1].color[2] = 1;
    meshes[2].indices.clear();
    r.render(meshes);
    const auto input = readTarget(r);
    const auto output = runFrames(r, meshes, false, 24);
    int minimum = 255, farError = 0;
    // Floor under the occluder stays visible only within ~0.32 of its -z edge.
    for (unsigned z = 0; z < 3; ++z) for (unsigned x = 0; x < 6; ++x) {
        const size_t p = pixelAt(2 + x * 0.15f, 0, -1.96f + z * 0.06f, camera());
        const size_t f = pixelAt(9 + x * 0.15f, 0, -1.96f + z * 0.06f, camera());
        require(input[p] == std::array<uint8_t, 4>{89, 89, 89, 255}, "occlusion sample outside visible receiver");
        require(input[f] == input[p], "occlusion control outside floor");
        for (unsigned c = 0; c < 3; ++c)
            minimum = std::min(minimum, int(input[p][c]) - output[p][c]);
        for (unsigned c = 0; c < 4; ++c)
            farError = std::max(farError, std::abs(int(input[f][c]) - output[f][c]));
    }
    const bool passed = minimum > 4 && farError <= 1;
    std::printf("%s RTGI %ux MSAA hidden underside occlusion: minimum darkening=%d far error=%d (limits >4/1)\n",
        passed ? "PASS" : "FAIL", r.samples, minimum, farError);
    return passed;
}

static bool verifyCutouts(Renderer &r)
{
    auto meshes = scene(true);
    meshes[0].color[0] = meshes[0].color[1] = meshes[0].color[2] = 0.35f;
    auto &occluder = meshes[1];
    occluder.vertices = {{1, 0.2f, -2, 0, 0, 0}, {10, 0.2f, -2, 0, 1, 0},
        {1, 0.2f, 2, 0, 0, 1}, {10, 0.2f, 2, 0, 1, 1}};
    occluder.color[0] = occluder.color[1] = occluder.color[2] = 1;
    occluder.draw.fvf |= 0x100; // UV0 follows the unused blend weight.
    occluder.draw.has_texture = true;
    occluder.draw.depth.alpha_test_enable = true;
    occluder.draw.depth.alpha_func = RECOMP_D3D_COMPARE_GREATER;
    occluder.draw.depth.alpha_ref = 128;
    meshes[2].indices.clear();
    const uint32_t texels[] = {0xffffffff, 0x00ffffff};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 2;
    desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{texels, sizeof texels, 0};
    ComPtr<ID3D11Texture2D> texture;
    check(r.device->CreateTexture2D(&desc, &data, &texture), "create cutout texture");
    check(r.device->CreateShaderResourceView(texture.Get(), nullptr, &occluder.texture), "create cutout SRV");
    r.render(meshes);
    const auto input = readTarget(r);
    const auto cutout = runFrames(r, meshes, false, 24);
    occluder.draw.depth.alpha_test_enable = false;
    r.render(meshes);
    require(readTarget(r) == input, "alpha-test toggle changed raster color");
    const auto solid = runFrames(r, meshes, false, 24);
    int opaqueMinimum = 255, transparentError = 0, solidMinimum[2] = {255, 255};
    // Both halves leave visible floor near -z; stay far from the alpha boundary.
    for (unsigned half = 0; half < 2; ++half)
        for (unsigned z = 0; z < 3; ++z) for (unsigned x = 0; x < 6; ++x) {
            const size_t p = pixelAt(2 + half * 6 + x * 0.15f, 0, -1.96f + z * 0.06f, camera());
            require(input[p] == std::array<uint8_t, 4>{89, 89, 89, 255}, "cutout sample outside visible receiver");
            for (unsigned c = 0; c < 3; ++c) {
                solidMinimum[half] = std::min(solidMinimum[half], int(input[p][c]) - solid[p][c]);
                if (!half) opaqueMinimum = std::min(opaqueMinimum, int(input[p][c]) - cutout[p][c]);
            }
            if (half) for (unsigned c = 0; c < 4; ++c)
                transparentError = std::max(transparentError, std::abs(int(input[p][c]) - cutout[p][c]));
        }
    const bool passed = opaqueMinimum > 4 && transparentError <= 1 && solidMinimum[0] > 4 && solidMinimum[1] > 4;
    std::printf("%s RTGI %ux MSAA alpha cutouts: opaque darkening=%d transparent error=%d disabled darkening=%d/%d (limits >4/1/>4/>4)\n",
        passed ? "PASS" : "FAIL", r.samples, opaqueMinimum, transparentError, solidMinimum[0], solidMinimum[1]);
    return passed;
}

// The wall is wholly outside this top-down camera, but its inward face is in
// capture. Every floor sample is at least 1.5 units away (contact range is .5).
static std::array<Mesh, 3> offscreenScene()
{
    auto meshes = scene(true);
    meshes[0].vertices = {{-5, 0, -5, 0}, {5, 0, -5, 0}, {5, 0, 5, 0}, {-5, 0, 5, 0}};
    meshes[0].color[0] = meshes[0].color[1] = meshes[0].color[2] = 0.5f;
    auto &wall = meshes[1];
    wall.vertices = {{-6, 0, -10, 0, 1.25f, 0}, {-6, 8, -10, 0, 1.75f, 0},
        {-6, 0, 10, 0, 1.25f, 1}, {-6, 8, 10, 0, 1.75f, 1}};
    wall.draw.fvf |= 0x100;
    wall.draw.has_texture = true;
    wall.draw.texture.linear = true;
    wall.draw.texture.width = 2;
    wall.draw.texture.height = 1;
    wall.draw.address_u = wall.draw.address_v = 3;
    wall.colorMaterial = true;
    meshes[2].indices.clear();
    XMFLOAT4X4 projection;
    XMStoreFloat4x4(&projection, XMMatrixLookAtLH(XMVectorSet(0, 10, 0, 1),
        XMVectorZero(), XMVectorSet(0, 0, 1, 0)) * XMMatrixOrthographicLH(10, 10, 0.1f, 60));
    for (auto &mesh : meshes) std::memcpy(mesh.draw.view_projection, &projection, sizeof projection);
    return meshes;
}

static ComPtr<ID3D11ShaderResourceView> colorTexture(ID3D11Device *device, uint32_t color)
{
    // A black first texel catches zeroed UVs and missing linear-coordinate scaling.
    const uint32_t texels[] = {0xff000000, color};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 2;
    desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{texels, sizeof texels, 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    check(device->CreateTexture2D(&desc, &data, &texture), "create bounce texture");
    check(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "create bounce SRV");
    return view;
}

static bool verifyOffscreenColor(Renderer &r)
{
    auto meshes = offscreenScene();
    auto &wall = meshes[1];
    wall.texture = colorTexture(r.device, 0xffff0000);
    r.render(meshes);
    const auto input = readTarget(r);
    const auto projection = XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4 *>(wall.draw.view_projection));
    for (const auto &v : wall.vertices) {
        const auto clip = XMVector4Transform(XMVectorSet(v.x, v.y, v.z, 1), projection);
        require(XMVectorGetX(clip) < -XMVectorGetW(clip), "wall is not outside camera view");
    }
    for (const auto &pixel : input)
        require(pixel[0] >= 127 && pixel[0] <= 128 && pixel[0] == pixel[1] &&
            pixel[1] == pixel[2] && pixel[3] == 255, "offscreen fixture lost neutral floor");
    const auto red = runFrames(r, meshes, false, 32);
    wall.texture = colorTexture(r.device, 0xff0000ff);
    const auto blue = runFrames(r, meshes, false, 32);
    _putenv_s("RECOMP_D3D_RTGI_HITCOLOR", "0");
    const auto disabled = runFrames(r, meshes, false, 32);
    _putenv_s("RECOMP_D3D_RTGI_HITCOLOR", "");
    const auto originalVertices = wall.vertices;
    const auto originalFvf = wall.draw.fvf;
    wall.draw.fvf = 0x112; // XYZ, normal, UV0.
    for (auto &v : wall.vertices) {
        v.extraU = v.u;
        v.extraV = v.v;
        v.weight = 1; v.u = v.v = 0; // Normal faces the floor.
    }
    auto &light = wall.draw.directional;
    light.enabled = true;
    light.count = 1;
    light.normal_transforms[0][0] = light.normal_transforms[0][5] = light.normal_transforms[0][10] = 1;
    light.ambient_emissive[0] = light.ambient_emissive[1] = light.ambient_emissive[2] = 1;
    const auto directional = runFrames(r, meshes, false, 32);
    light.positions[0][3] = 1;
    const auto point = runFrames(r, meshes, false, 32);
    light = {};
    wall.draw.fvf = originalFvf;
    wall.vertices = originalVertices;
    wall.draw.texture.linear = false;
    for (auto &v : wall.vertices) v.u *= 0.5f;
    const auto normalized = runFrames(r, meshes, false, 32);
    wall.draw.address_u = 1;
    for (auto &v : wall.vertices) v.u += 1;
    const auto wrapped = runFrames(r, meshes, false, 32);
    wall.draw.address_u = 2;
    for (auto &v : wall.vertices) v.u = 3 - v.u;
    const auto mirrored = runFrames(r, meshes, false, 32);
    wall.draw.address_u = 3;
    for (auto &v : wall.vertices) v.u = 2 - v.u;
    wall.draw.depth.alpha_test_enable = true;
    wall.draw.depth.alpha_func = RECOMP_D3D_COMPARE_GREATER;
    wall.draw.depth.alpha_ref = 128;
    wall.texture = colorTexture(r.device, 0x000000ff);
    const auto hole = runFrames(r, meshes, false, 32);
    wall.texture = colorTexture(r.device, 0xff0000ff);
    wall.draw.depth.alpha_test_enable = false;
    wall.draw.zero_diffuse_rgb = true;
    const auto unlit = runFrames(r, meshes, false, 32);
    wall.draw.zero_diffuse_rgb = false;
    wall.draw.modulate_texture_factor = true;
    wall.draw.texture_factor = 0xff000000;
    const auto tinted = runFrames(r, meshes, false, 32);
    wall.draw.modulate_texture_factor = false;
    wall.draw.four_tap_filter = true;
    const auto unsupported = runFrames(r, meshes, false, 32);
    wall.draw.four_tap_filter = false;
    wall.colorMaterial = false;
    const auto unclassified = runFrames(r, meshes, false, 32);
    require(disabled == unclassified, "disabled hit color differs from contact fallback");
    wall.indices.clear();
    const auto absent = runFrames(r, meshes, false, 32);
    double change[2][3]{};
    unsigned count = 0;
    int removedError = 0, fallbackError = 0, unlitHue = 0, tintError = 0, uvError = 0, holeError = 0;
    int unlitDarkening = 255, directionalError = 0, pointError = 0;
    // Left floor strip x=-4.5..-2.5, away from image/filter boundaries.
    for (UINT y = height / 4; y < 3 * height / 4; ++y)
        for (UINT x = width / 20; x < width / 4; ++x) {
            const size_t p = size_t(y) * width + x;
            for (unsigned c = 0; c < 3; ++c) {
                change[0][2 - c] += int(red[p][c]) - input[p][c];
                change[1][2 - c] += int(blue[p][c]) - input[p][c];
                removedError = std::max(removedError, std::abs(int(absent[p][c]) - input[p][c]));
                fallbackError = std::max({fallbackError, std::abs(int(unsupported[p][c]) - input[p][c]),
                    std::abs(int(unclassified[p][c]) - input[p][c])});
                unlitHue = std::max(unlitHue, std::abs(int(unlit[p][c]) - unlit[p][0]));
                tintError = std::max(tintError, std::abs(int(tinted[p][c]) - unlit[p][c]));
                uvError = std::max({uvError, std::abs(int(normalized[p][c]) - blue[p][c]),
                    std::abs(int(wrapped[p][c]) - blue[p][c]), std::abs(int(mirrored[p][c]) - blue[p][c])});
                holeError = std::max(holeError, std::abs(int(hole[p][c]) - input[p][c]));
                unlitDarkening = std::min(unlitDarkening, int(input[p][c]) - unlit[p][c]);
                directionalError = std::max(directionalError, std::abs(int(directional[p][c]) - blue[p][c]));
                pointError = std::max(pointError, std::abs(int(point[p][c]) - input[p][c]));
            }
            ++count;
        }
    for (auto &rgb : change) for (auto &channel : rgb) channel /= count;
    const bool passed = change[0][0] > 2 && change[0][0] > change[0][2] + 5 &&
        change[1][2] > 2 && change[1][2] > change[1][0] + 5 &&
        removedError <= 1 && fallbackError <= 1 && unlitHue <= 1 && tintError <= 1 &&
        uvError <= 1 && holeError <= 1 && unlitDarkening > 4 && directionalError <= 1 && pointError <= 1;
    std::printf("%s RTGI %ux MSAA offscreen texture bounce (32 frames): red RGB=(%.2f,%.2f,%.2f) "
        "blue RGB=(%.2f,%.2f,%.2f) removed=%d unsupported=%d unlit hue=%d tint error=%d UV error=%d hole=%d dark=%d directional=%d point fallback=%d\n",
        passed ? "PASS" : "FAIL", r.samples, change[0][0], change[0][1], change[0][2],
        change[1][0], change[1][1], change[1][2], removedError, fallbackError, unlitHue, tintError,
        uvError, holeError, unlitDarkening, directionalError, pointError);
    return passed;
}

static double gpuTime(Renderer &r, std::array<Mesh, 3> meshes, bool moving)
{
    auto *device = r.device;
    auto *context = r.context;
    Rtgi gi(d3d_rtgi_create(device, r.width, r.height), d3d_rtgi_destroy);
    require(bool(gi), "create timing module");
    ComPtr<ID3D11Query> disjoint, begin, end;
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    check(device->CreateQuery(&desc, &disjoint), "create disjoint query");
    desc.Query = D3D11_QUERY_TIMESTAMP;
    check(device->CreateQuery(&desc, &begin), "create start timestamp");
    check(device->CreateQuery(&desc, &end), "create end timestamp");
    std::vector<double> times;
    for (unsigned frame = 0; frame < 24; ++frame) {
        if (moving) moveSphere(meshes[2], frame);
        r.render(meshes);
        addScene(gi.get(), meshes, false);
        context->Begin(disjoint.Get());
        context->End(begin.Get());
        apply(gi.get(), r);
        context->End(end.Get());
        context->End(disjoint.Get());
        waitReadback(r);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
        UINT64 start = 0, finish = 0;
        require(context->GetData(disjoint.Get(), &frequency, sizeof frequency, 0) == S_OK &&
            context->GetData(begin.Get(), &start, sizeof start, 0) == S_OK &&
            context->GetData(end.Get(), &finish, sizeof finish, 0) == S_OK, "timestamps unavailable");
        require(!frequency.Disjoint && frequency.Frequency && finish >= start, "disjoint GPU timestamps");
        if (frame >= 16) times.push_back(1000.0 * (finish - start) / frequency.Frequency);
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

static bool verifyGpuTime(ID3D11Device *device, ID3D11DeviceContext *context)
{
    Renderer r(device, context, 1, 1920, 1080);
    const double milliseconds = gpuTime(r, movingScene(), true);
    const bool passed = milliseconds < 3.0;
    std::printf("%s RTGI whole apply 1920x1080 GPU=%.3f ms (median of 8, limit 3 ms)\n",
        passed ? "PASS" : "FAIL", milliseconds);
    Renderer large(device, context, 1, 3840, 2160);
    auto meshes = offscreenScene();
    meshes[1].texture = colorTexture(device, 0xffff0000);
    meshes[1].colorMaterial = false;
    const double contact = gpuTime(large, meshes, false);
    meshes[1].colorMaterial = true;
    const double color = gpuTime(large, meshes, false);
    std::printf("RTGI whole apply 3840x2160 1x MSAA GPU: contact=%.3f ms color=%.3f ms change=%+.1f%% "
        "(median of 8 after 16 warmup frames, target <=10%%)\n", contact, color, 100 * (color / contact - 1));
    return passed;
}

static bool debugMessages(ID3D11InfoQueue *q11, ID3D12InfoQueue *q12)
{
    bool passed = true;
    if (q11) for (UINT64 i = 0; i < q11->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0;
        check(q11->GetMessage(i, nullptr, &size), "size D3D11 debug message");
        std::vector<unsigned char> storage(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
        check(q11->GetMessage(i, message, &size), "read D3D11 debug message");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) std::fprintf(stderr, "D3D11: %s\n", message->pDescription);
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) passed = false;
    }
    if (q12) for (UINT64 i = 0; i < q12->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0;
        check(q12->GetMessage(i, nullptr, &size), "size D3D12 debug message");
        std::vector<unsigned char> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        check(q12->GetMessage(i, message, &size), "read D3D12 debug message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) std::fprintf(stderr, "D3D12: %s\n", message->pDescription);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) passed = false;
    }
    std::puts(passed ? "PASS RTGI debug layers: no errors" : "FAIL RTGI debug layers");
    return passed;
}

// A game-sized frame: a dense floor, a wall and 12 spheres, half of them skinned.
static std::vector<Mesh> benchScene(UINT w, UINT h)
{
    std::vector<Mesh> meshes(14);
    auto &floor = meshes[0];
    constexpr unsigned cells = 200;
    for (unsigned z = 0; z <= cells; ++z) for (unsigned x = 0; x <= cells; ++x)
        floor.vertices.push_back({-20 + 40.0f * x / cells, 0, -20 + 40.0f * z / cells, 0, 0, 1, 0});
    for (unsigned z = 0; z < cells; ++z) for (unsigned x = 0; x < cells; ++x) {
        const uint16_t a = uint16_t(z * (cells + 1) + x), b = uint16_t(a + cells + 1);
        floor.indices.insert(floor.indices.end(), {a, b, uint16_t(a + 1), uint16_t(a + 1), b, uint16_t(b + 1)});
    }
    floor.color[0] = floor.color[1] = floor.color[2] = 0.7f;
    auto &wall = meshes[1];
    wall.vertices = {{-20, 0, 12, 0, 0, 0, -1}, {-20, 8, 12, 0, 0, 0, -1}, {20, 0, 12, 0, 0, 0, -1}, {20, 8, 12, 0, 0, 0, -1}};
    wall.indices = {0, 1, 2, 2, 1, 3};
    wall.color[0] = 0.8f; wall.color[1] = 0.2f; wall.color[2] = 0.1f;
    constexpr unsigned rings = 48, segments = 96;
    for (unsigned s = 2; s < meshes.size(); ++s) {
        auto &sphere = meshes[s];
        for (unsigned y = 0; y <= rings; ++y) for (unsigned x = 0; x <= segments; ++x) {
            const float latitude = XM_PI * y / rings, longitude = XM_2PI * x / segments;
            const float nx = std::sin(latitude) * std::cos(longitude), ny = std::cos(latitude),
                nz = std::sin(latitude) * std::sin(longitude);
            sphere.vertices.push_back({1.2f * nx, 1.2f * ny, 1.2f * nz, 0.5f + 0.4f * ny, nx, ny, nz});
        }
        for (unsigned y = 0; y < rings; ++y) for (unsigned x = 0; x < segments; ++x) {
            const uint16_t a = uint16_t(y * (segments + 1) + x), b = uint16_t(a + segments + 1);
            sphere.indices.insert(sphere.indices.end(), {a, b, uint16_t(a + 1), uint16_t(a + 1), b, uint16_t(b + 1)});
        }
        sphere.draw.blend_weight_count = s % 2;
        sphere.color[0] = 0.6f; sphere.color[1] = 0.45f; sphere.color[2] = 0.35f;
    }
    XMFLOAT4X4 matrix;
    XMStoreFloat4x4(&matrix, XMMatrixLookAtLH(XMVectorSet(0, 5, -14, 1), XMVectorSet(0, 1, 2, 1),
        XMVectorSet(0, 1, 0, 0)) * XMMatrixPerspectiveFovLH(0.9f, float(w) / h, 0.1f, 400));
    for (auto &mesh : meshes) {
        mesh.color[3] = 1;
        auto &draw = mesh.draw;
        draw.has_transform = true;
        draw.depth.depth_write_enable = true;
        draw.primitive_type = 5;
        draw.cull_mode = RECOMP_D3D_CULL_NONE;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x016; // XYZB1 | NORMAL: the normal sits in u, v, extraU.
        draw.vertex_count = UINT(mesh.vertices.size());
        draw.triangle_count = draw.index_count = UINT(mesh.indices.size());
        draw.triangle_count /= 3;
        draw.vertex_bytes = mesh.vertices.data();
        draw.index_bytes = mesh.indices.data();
        for (auto &world : draw.world) world[0] = world[5] = world[10] = world[15] = 1;
        std::memcpy(draw.view_projection, &matrix, sizeof matrix);
    }
    return meshes;
}

static void moveBench(std::vector<Mesh> &meshes, unsigned frame)
{
    for (unsigned s = 2; s < meshes.size(); ++s) for (unsigned bone = 0; bone < 2; ++bone) {
        const float x = -10 + 4.0f * ((s - 2) % 6), z = (s - 2) < 6 ? 0.0f : 5.0f;
        XMFLOAT4X4 world;
        XMStoreFloat4x4(&world, XMMatrixRotationY(0.05f * frame + 0.1f * bone) *
            XMMatrixTranslation(x + 0.3f * std::sin(0.2f * frame + s), 1.2f, z));
        std::memcpy(meshes[s].draw.world[bone], &world, sizeof world);
    }
}

// Prints per-stage cost at the player's 4K/8x MSAA settings. Not a pass/fail gate.
static int benchmark(ID3D11Device *device, ID3D11DeviceContext *context, UINT w, UINT h, UINT samples)
{
    Renderer r(device, context, samples, w, h);
    auto meshes = benchScene(w, h);
    Rtgi gi(d3d_rtgi_create(device, w, h, true), d3d_rtgi_destroy);
    require(bool(gi), "create benchmark module");
    ComPtr<ID3D11Query> disjoint, begin, end;
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    check(device->CreateQuery(&desc, &disjoint), "create disjoint query");
    desc.Query = D3D11_QUERY_TIMESTAMP;
    check(device->CreateQuery(&desc, &begin), "create start timestamp");
    check(device->CreateQuery(&desc, &end), "create end timestamp");
    constexpr unsigned warmup = 16, frames = 64;
    double whole = 0;
    for (unsigned frame = 0; frame < warmup + frames; ++frame) {
        if (frame == warmup) d3d_rtgi_take_stats(gi.get());
        moveBench(meshes, frame);
        r.render(meshes);
        for (auto &mesh : meshes) d3d_rtgi_add_draw(gi.get(), mesh.draw, nullptr, nullptr);
        context->Begin(disjoint.Get());
        context->End(begin.Get());
        apply(gi.get(), r);
        context->End(end.Get());
        context->End(disjoint.Get());
        waitReadback(r);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
        UINT64 start = 0, finish = 0;
        require(context->GetData(disjoint.Get(), &frequency, sizeof frequency, 0) == S_OK &&
            context->GetData(begin.Get(), &start, sizeof start, 0) == S_OK &&
            context->GetData(end.Get(), &finish, sizeof finish, 0) == S_OK, "timestamps unavailable");
        if (frame >= warmup) whole += 1000.0 * (finish - start) / frequency.Frequency;
    }
    const auto s = d3d_rtgi_take_stats(gi.get());
    const double traced = std::max(1u, s.traced), d12 = std::max(1u, s.d3d12Passes), d11 = std::max(1u, s.d3d11Passes);
    std::printf("RTGI bench %ux%u %ux MSAA, %u frames: GPU apply=%.3f ms | traced=%u reused=%u tris=%.0f "
        "| CPU collect=%.3f apply=%.3f wait=%.3f ms | GPU build=%.3f trace=%.3f temporal=%.3f spatial=%.3f "
        "composite=%.3f sync=%.3f ms (%u/%u timed)\n", w, h, samples, frames, whole / frames,
        s.traced, s.reused, s.triangles / traced, s.collectMs / frames, s.applyMs / frames, s.waitMs / frames,
        s.buildMs / d12, s.traceMs / d12, s.temporalMs / d12, s.spatialMs / d12, s.compositeMs / d11,
        s.syncMs / d11, s.d3d12Passes, s.d3d11Passes);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context))) return 77;
        try {
            benchmark(device.Get(), context.Get(), 3840, 2160, 8);
            benchmark(device.Get(), context.Get(), 3840, 2160, 1);
            return 0;
        } catch (...) {
            std::fputs("FAIL RTGI benchmark\n", stderr);
            return 1;
        }
    }
    ComPtr<ID3D11InfoQueue> messages11;
    ComPtr<ID3D12InfoQueue> messages12;
    try {
        const ULONGLONG started = GetTickCount64();
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        HRESULT created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
        const bool debug11 = created != DXGI_ERROR_SDK_COMPONENT_MISSING;
        if (!debug11) created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(created)) {
            std::printf("SKIP no hardware D3D11 device (0x%08lX)\n", static_cast<unsigned long>(created));
            return 77;
        }
        if (debug11) check(device.As(&messages11), "get D3D11 debug queue");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        check(device.As(&dxgi), "get DXGI device");
        check(dxgi->GetAdapter(&adapter), "get adapter");
        // GPU-less CI still gets a "hardware" device on Microsoft's software renderer.
        ComPtr<IDXGIAdapter1> adapter1;
        DXGI_ADAPTER_DESC1 adapterDesc{};
        if (SUCCEEDED(adapter.As(&adapter1)) && SUCCEEDED(adapter1->GetDesc1(&adapterDesc)) &&
            ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || adapterDesc.VendorId == 0x1414)) {
            std::puts("SKIP RTGI: no hardware GPU");
            return 77;
        }
        ComPtr<ID3D12Device> device12;
        const HRESULT created12 = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device12));
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
        const bool supported = SUCCEEDED(created12) &&
            SUCCEEDED(device12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof options)) &&
            SUCCEEDED(device12->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof model)) &&
            options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 && model.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
        if (device12 && debug) check(device12.As(&messages12), "get D3D12 debug queue");
        // D3D12CreateDevice returns the adapter's existing device, including its info queue.
        Rtgi probe(d3d_rtgi_create(device.Get(), width, height), d3d_rtgi_destroy);
        if (!probe && !supported) { std::puts("SKIP RTGI: hardware DXR 1.1 / SM 6.5 unavailable"); return 77; }
        require(bool(probe), "RTGI creation failed on DXR-capable hardware");
        probe.reset();
        require(debug11 && debug, "D3D11/D3D12 debug layers unavailable");
        std::printf("RTGI debug layers: D3D11=%s D3D12=%s\n", debug11 ? "enabled" : "unavailable", debug ? "enabled" : "unavailable");
        // 1.5 offsets the composite's highlight fade on these mid-grey
        // receivers, keeping each bounce several 8-bit levels above rounding.
        require(_putenv_s("RECOMP_D3D_RTGI_STRENGTH", "1.5") == 0, "set strength");
        bool passed = true;
        for (UINT samples : {1u, 4u}) {
            Renderer renderer(device.Get(), context.Get(), samples);
            auto meshes = scene(false);
            renderer.render(meshes);
            auto input = readback(renderer, renderer.colorSRV.Get());
            auto output = runFrames(renderer, meshes, true);
            passed = verifyImage(input, output, samples, camera()) && passed;
            auto ranged = scene(false);
            for (auto &mesh : ranged) {
                // Unreferenced NaNs must neither be transformed nor reject a valid draw.
                mesh.vertices.insert(mesh.vertices.begin(), 512, Vertex{NAN, NAN, NAN});
                for (auto &index : mesh.indices) index += 512;
                mesh.draw.vertex_count = UINT(mesh.vertices.size());
            }
            require(runFrames(renderer, ranged, false) == output,
                "indexed vertex range changed skinning/strip/fan output");
            auto baked = scene(true);
            const auto reference = runFrames(renderer, baked, false);
            unsigned difference = 0;
            for (size_t p = 0; p < output.size(); ++p) for (unsigned c = 0; c < 4; ++c)
                difference = std::max(difference, unsigned(std::abs(int(output[p][c]) - reference[p][c])));
            const bool geometry = difference <= 1;
            std::printf("%s RTGI %ux MSAA skinning/strip/fan/filter reference max error=%u\n", geometry ? "PASS" : "FAIL", samples, difference);
            passed = geometry && passed;
            std::printf("PASS RTGI %ux MSAA add_draw acceptance / exclusions\n", samples);
            require(_putenv_s("RECOMP_D3D_RTGI_STRENGTH", "0") == 0, "set zero strength");
            const bool zero = runFrames(renderer, meshes, false, 4) == input;
            std::printf("%s RTGI %ux MSAA zero strength / input preservation / empty fallback\n", zero ? "PASS" : "FAIL", samples);
            passed = zero && passed;
            if (samples == 4) passed = verifySamples(renderer) && passed;
            require(_putenv_s("RECOMP_D3D_RTGI_STRENGTH", "1.5") == 0, "restore strength");
            passed = verifyHistory(renderer) && passed;
            passed = verifyWeakLighting(renderer) && passed;
            passed = verifyBanding(renderer) && passed;
            passed = verifyMovingNoise(renderer) && passed;
            passed = verifyFireflies(renderer) && passed;
            passed = verifyOcclusion(renderer) && passed;
            passed = verifyCutouts(renderer) && passed;
            passed = verifyOffscreenColor(renderer) && passed;
        }
        passed = verifyGpuTime(device.Get(), context.Get()) && passed;
        passed = debugMessages(messages11.Get(), messages12.Get()) && passed;
        std::printf("%s hardware RTGI module (%llu ms)\n", passed ? "PASS" : "FAIL",
            static_cast<unsigned long long>(GetTickCount64() - started));
        return passed ? 0 : 1;
    } catch (HRESULT) {
        debugMessages(messages11.Get(), messages12.Get());
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        debugMessages(messages11.Get(), messages12.Get());
    }
    return 1;
}
