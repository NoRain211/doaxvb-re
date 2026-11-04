#include "d3d_dlss.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <chrono>
#include <cstdio>
#include <cwchar>

struct D3dDlss {
    HMODULE module = nullptr;
    DlssBridgeEvaluate evaluate = nullptr;
    DlssBridgeShutdown shutdown = nullptr;  // set once the bridge is initialized
    DlssBridgeError error = nullptr;
    uint32_t output_width = 0u, output_height = 0u;
    ID3D11Texture2D *output = nullptr;       // output-sized RGBA8 with unordered access
    ID3D11ShaderResourceView *output_view = nullptr;
    ID3D11VertexShader *copy_vs = nullptr;
    ID3D11PixelShader *copy_ps = nullptr;
    ID3D11SamplerState *sampler = nullptr;
    double last_ms = 0.0;
    bool reported = false;
};

namespace {

template <typename T>
void release(T *&object)
{
    if (object != nullptr) object->Release();
    object = nullptr;
}

double steadyMs()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

void d3d_dlss_destroy(D3dDlss *dlss)
{
    if (dlss == nullptr) return;
    release(dlss->output);
    release(dlss->output_view);
    release(dlss->copy_vs);
    release(dlss->copy_ps);
    release(dlss->sampler);
    if (dlss->shutdown != nullptr) dlss->shutdown();  // NGX needs the device it was created on
    if (dlss->module != nullptr) FreeLibrary(dlss->module);
    delete dlss;
}

D3dDlss *d3d_dlss_create(ID3D11Device *device, ID3D11DeviceContext *context, int mode,
    uint32_t output_width, uint32_t output_height, uint32_t *render_width, uint32_t *render_height)
{
    auto *dlss = new D3dDlss;
    const auto off = [&dlss](const char *why) -> D3dDlss * {
        std::fprintf(stderr, "recomp dlss: %s; off\n", why);
        d3d_dlss_destroy(dlss);
        return nullptr;
    };
    dlss->module = LoadLibraryExW(L"dlss_bridge.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    if (dlss->module == nullptr) return off("dlss_bridge.dll not found beside the runner");
    const auto symbol = [dlss](const char *name) {
        return reinterpret_cast<void *>(GetProcAddress(dlss->module, name));
    };
    const auto init = reinterpret_cast<DlssBridgeInit>(symbol("dlss_bridge_init"));
    const auto render_size = reinterpret_cast<DlssBridgeRenderSize>(symbol("dlss_bridge_render_size"));
    const auto create = reinterpret_cast<DlssBridgeCreate>(symbol("dlss_bridge_create"));
    dlss->evaluate = reinterpret_cast<DlssBridgeEvaluate>(symbol("dlss_bridge_evaluate"));
    dlss->error = reinterpret_cast<DlssBridgeError>(symbol("dlss_bridge_error"));
    const auto shutdown = reinterpret_cast<DlssBridgeShutdown>(symbol("dlss_bridge_shutdown"));
    if (!init || !render_size || !create || !dlss->evaluate || !dlss->error || !shutdown)
        return off("dlss_bridge.dll is incomplete");
    wchar_t directory[MAX_PATH] = L".";
    const DWORD length = GetModuleFileNameW(nullptr, directory, MAX_PATH);
    if (length != 0u && length < MAX_PATH) {
        wchar_t *slash = std::wcsrchr(directory, L'\\');
        if (slash != nullptr) *slash = L'\0';
    }
    if (!init(device, directory)) return off(dlss->error());
    dlss->shutdown = shutdown;

    unsigned width = output_width, height = output_height;
    if (mode != DLSS_BRIDGE_DLAA && !render_size(output_width, output_height, mode, &width, &height))
        return off(dlss->error());

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = output_width;
    desc.Height = output_height;
    desc.MipLevels = desc.ArraySize = 1u;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1u;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    HRESULT result = device->CreateTexture2D(&desc, nullptr, &dlss->output);
    if (SUCCEEDED(result)) result = device->CreateShaderResourceView(dlss->output, nullptr, &dlss->output_view);
    static const char shader[] =
        "Texture2D pixels : register(t0);\n"
        "SamplerState bilinear : register(s0);\n"
        "float4 vs(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position {\n"
        " uv = float2((id << 1) & 2, id & 2);\n"
        " return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
        "float4 ps(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        " return float4(pixels.SampleLevel(bilinear, uv, 0).rgb, 1); }\n";
    ID3DBlob *vertex = nullptr, *pixel = nullptr;
    if (SUCCEEDED(result)) result = D3DCompile(shader, sizeof shader - 1u, nullptr, nullptr, nullptr,
        "vs", "vs_4_0", 0u, 0u, &vertex, nullptr);
    if (SUCCEEDED(result)) result = D3DCompile(shader, sizeof shader - 1u, nullptr, nullptr, nullptr,
        "ps", "ps_4_0", 0u, 0u, &pixel, nullptr);
    if (SUCCEEDED(result)) result = device->CreateVertexShader(
        vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &dlss->copy_vs);
    if (SUCCEEDED(result)) result = device->CreatePixelShader(
        pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &dlss->copy_ps);
    release(vertex);
    release(pixel);
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(result)) result = device->CreateSamplerState(&sampler, &dlss->sampler);
    if (FAILED(result)) return off("could not create its targets");
    if (!create(context, width, height, output_width, output_height, mode)) return off(dlss->error());
    dlss->output_width = output_width;
    dlss->output_height = output_height;
    *render_width = width;
    *render_height = height;
    return dlss;
}

void d3d_dlss_resolve(D3dDlss *dlss, ID3D11DeviceContext *context, DlssBridgeFrame frame,
    ID3D11ShaderResourceView *scene, ID3D11RenderTargetView *target)
{
    context->ClearState();
    const double now = steadyMs();
    frame.output = dlss->output;
    frame.frame_ms = dlss->last_ms != 0.0 ? static_cast<float>(now - dlss->last_ms) : 16.7f;
    dlss->last_ms = now;
    const bool upscaled = dlss->evaluate(context, &frame) != 0;
    if (!upscaled && !dlss->reported) {
        dlss->reported = true;
        std::fprintf(stderr, "recomp dlss: %s; scaling the scene bilinearly\n", dlss->error());
    }
    context->ClearState();
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(dlss->output_width),
        static_cast<float>(dlss->output_height), 0.0f, 1.0f};
    context->RSSetViewports(1u, &viewport);
    context->OMSetRenderTargets(1u, &target, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(dlss->copy_vs, nullptr, 0u);
    context->PSSetShader(dlss->copy_ps, nullptr, 0u);
    context->PSSetSamplers(0u, 1u, &dlss->sampler);
    ID3D11ShaderResourceView *source = upscaled ? dlss->output_view : scene;
    context->PSSetShaderResources(0u, 1u, &source);
    context->Draw(3u, 0u);
    context->ClearState();
}
