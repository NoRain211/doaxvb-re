#include "d3d_fsr.h"

#include <d3dcompiler.h>

#include <cstdint>
#include <cstdio>
#include <string>

#ifdef RECOMP_FSR1
// AMD FidelityFX FSR 1.0 (third_party/fsr1, MIT): the CPU half sets the
// shader constants; the GPU half is embedded and compiled at startup.
#include <math.h>
#include <stdlib.h>
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"
#undef A_CPU
static const char kFfxASource[] = {
#include "ffx_a_hlsl.inc"
};
static const char kFfxFsr1Source[] = {
#include "ffx_fsr1_hlsl.inc"
};
#endif

struct D3dFsr {
    float sharpness = 0.2f;
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *easu = nullptr;
    ID3D11PixelShader *rcas = nullptr;
    ID3D11Buffer *constants = nullptr;
    ID3D11SamplerState *sampler = nullptr;
    ID3D11RenderTargetView *target = nullptr; // EASU output at the letterbox size
    ID3D11ShaderResourceView *source = nullptr;
    uint32_t width = 0u, height = 0u;
};

namespace {

template <typename T>
void release(T *&object)
{
    if (object != nullptr) object->Release();
    object = nullptr;
}

struct FsrConstants {
    uint32_t con[4][4];
    int32_t origin[2];  // output pixel of the pass's first texel
    int32_t limit[2];   // RCAS source size minus one, for edge clamping
};

} // namespace

void d3d_fsr_destroy(D3dFsr *fsr)
{
    if (fsr == nullptr) return;
    release(fsr->vs);
    release(fsr->easu);
    release(fsr->rcas);
    release(fsr->constants);
    release(fsr->sampler);
    release(fsr->target);
    release(fsr->source);
    delete fsr;
}

/* FSR 1.0 needs Gather (feature level 11_0). EASU and RCAS share one source:
   the input callbacks the FFX headers declare are defined between them. */
D3dFsr *d3d_fsr_create(ID3D11Device *device, float sharpness)
{
#ifdef RECOMP_FSR1
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        std::fprintf(stderr, "recomp d3d presenter: fsr needs feature level 11_0; using bilinear\n");
        return nullptr;
    }
    std::string source = "#define A_GPU 1\n#define A_HLSL 1\n";
    source.append(kFfxASource, sizeof kFfxASource);
    source +=
        "\nTexture2D<float4> pixels : register(t0);\n"
        "SamplerState bilinear : register(s0);\n"
        "cbuffer Fsr : register(b0) { uint4 con0, con1, con2, con3; int2 origin, limit; };\n"
        "#define FSR_EASU_F 1\n#define FSR_RCAS_F 1\n"
        "AF4 FsrEasuRF(AF2 p) { return pixels.GatherRed(bilinear, p); }\n"
        "AF4 FsrEasuGF(AF2 p) { return pixels.GatherGreen(bilinear, p); }\n"
        "AF4 FsrEasuBF(AF2 p) { return pixels.GatherBlue(bilinear, p); }\n"
        "AF4 FsrRcasLoadF(ASU2 p) { return pixels.Load(int3(clamp(p, int2(0, 0), limit), 0)); }\n"
        "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n";
    source.append(kFfxFsr1Source, sizeof kFfxFsr1Source);
    source +=
        "\nfloat4 vs(uint id : SV_VertexID) : SV_Position {\n"
        " float2 uv = float2((id << 1) & 2, id & 2);\n"
        " return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
        "float4 easu(float4 p : SV_Position) : SV_Target {\n"
        " AF3 c; FsrEasuF(c, AU2(int2(p.xy) - origin), con0, con1, con2, con3); return float4(c, 1); }\n"
        "float4 rcas(float4 p : SV_Position) : SV_Target {\n"
        " AF1 r, g, b; FsrRcasF(r, g, b, AU2(int2(p.xy) - origin), con0); return float4(r, g, b, 1); }\n";
    auto *fsr = new D3dFsr;
    fsr->sharpness = sharpness;
    ID3DBlob *code[3]{}, *errors = nullptr;
    const char *entries[3] = {"vs", "easu", "rcas"};
    const char *profiles[3] = {"vs_5_0", "ps_5_0", "ps_5_0"};
    HRESULT result = S_OK;
    for (int i = 0; i < 3 && SUCCEEDED(result); ++i) {
        result = D3DCompile(source.data(), source.size(), "fsr1.hlsl", nullptr, nullptr,
            entries[i], profiles[i], D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u, &code[i], &errors);
        if (FAILED(result) && errors != nullptr) {
            std::fprintf(stderr, "recomp d3d presenter: fsr %s: %.*s\n", entries[i],
                static_cast<int>(errors->GetBufferSize()),
                static_cast<const char *>(errors->GetBufferPointer()));
        }
        release(errors);
    }
    if (SUCCEEDED(result)) result = device->CreateVertexShader(
        code[0]->GetBufferPointer(), code[0]->GetBufferSize(), nullptr, &fsr->vs);
    if (SUCCEEDED(result)) result = device->CreatePixelShader(
        code[1]->GetBufferPointer(), code[1]->GetBufferSize(), nullptr, &fsr->easu);
    if (SUCCEEDED(result)) result = device->CreatePixelShader(
        code[2]->GetBufferPointer(), code[2]->GetBufferSize(), nullptr, &fsr->rcas);
    for (auto *blob : code) release(blob);
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(FsrConstants);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(result)) result = device->CreateBuffer(&buffer, nullptr, &fsr->constants);
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(result)) result = device->CreateSamplerState(&sampler, &fsr->sampler);
    if (FAILED(result)) {
        std::fprintf(stderr, "recomp d3d presenter: fsr unavailable hr=0x%08lX; using bilinear\n",
            static_cast<unsigned long>(result));
        d3d_fsr_destroy(fsr);
        return nullptr;
    }
    std::fprintf(stderr, "recomp d3d presenter: fsr 1.0 on, sharpness %.2f\n", sharpness);
    return fsr;
#else
    (void)device;
    (void)sharpness;
    std::fprintf(stderr, "recomp d3d presenter: fsr not built into this runner; using bilinear\n");
    return nullptr;
#endif
}

bool d3d_fsr_scale(D3dFsr *fsr, ID3D11DeviceContext *context, ID3D11ShaderResourceView *source,
    ID3D11RenderTargetView *output, const D3D11_VIEWPORT &viewport)
{
#ifdef RECOMP_FSR1
    if (fsr == nullptr) return false;
    D3D11_TEXTURE2D_DESC desc{};
    {
        ID3D11Resource *resource = nullptr;
        source->GetResource(&resource);
        static_cast<ID3D11Texture2D *>(resource)->GetDesc(&desc);
        release(resource);
    }
    const uint32_t source_width = desc.Width, source_height = desc.Height;
    const uint32_t width = static_cast<uint32_t>(viewport.Width);
    const uint32_t height = static_cast<uint32_t>(viewport.Height);
    if (width < source_width || height < source_height ||
        (width == source_width && height == source_height)) {
        return false;
    }
    if (fsr->width != width || fsr->height != height) {
        release(fsr->target);
        release(fsr->source);
        fsr->width = fsr->height = 0u;
        ID3D11Device *device = nullptr;
        context->GetDevice(&device);
        desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1u;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1u;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D *texture = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) &&
            SUCCEEDED(device->CreateRenderTargetView(texture, nullptr, &fsr->target)) &&
            SUCCEEDED(device->CreateShaderResourceView(texture, nullptr, &fsr->source))) {
            fsr->width = width;
            fsr->height = height;
        }
        release(texture);
        release(device);
    }
    if (fsr->width == 0u) return false;
    FsrConstants constants{};
    FsrEasuCon(constants.con[0], constants.con[1], constants.con[2], constants.con[3],
        static_cast<AF1>(source_width), static_cast<AF1>(source_height),
        static_cast<AF1>(source_width), static_cast<AF1>(source_height),
        static_cast<AF1>(width), static_cast<AF1>(height));
    context->UpdateSubresource(fsr->constants, 0u, nullptr, &constants, 0u, 0u);
    const D3D11_VIEWPORT easu_viewport = {0.0f, 0.0f,
        static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->PSSetSamplers(0u, 1u, &fsr->sampler);
    context->RSSetViewports(1u, &easu_viewport);
    context->OMSetRenderTargets(1u, &fsr->target, nullptr);
    context->VSSetShader(fsr->vs, nullptr, 0u);
    context->PSSetShader(fsr->easu, nullptr, 0u);
    context->PSSetConstantBuffers(0u, 1u, &fsr->constants);
    context->PSSetShaderResources(0u, 1u, &source);
    context->Draw(3u, 0u);
    // UI sharpness 1 is RCAS's maximum (0 stops); 0 is two stops softer.
    FsrRcasCon(constants.con[0], 2.0f * (1.0f - fsr->sharpness));
    constants.origin[0] = static_cast<int32_t>(viewport.TopLeftX);
    constants.origin[1] = static_cast<int32_t>(viewport.TopLeftY);
    constants.limit[0] = static_cast<int32_t>(width) - 1;
    constants.limit[1] = static_cast<int32_t>(height) - 1;
    context->UpdateSubresource(fsr->constants, 0u, nullptr, &constants, 0u, 0u);
    ID3D11ShaderResourceView *none = nullptr;
    context->PSSetShaderResources(0u, 1u, &none);
    context->RSSetViewports(1u, &viewport);
    context->OMSetRenderTargets(1u, &output, nullptr);
    context->PSSetShader(fsr->rcas, nullptr, 0u);
    context->PSSetShaderResources(0u, 1u, &fsr->source);
    context->Draw(3u, 0u);
    context->ClearState();
    return true;
#else
    (void)fsr;
    (void)context;
    (void)source;
    (void)output;
    (void)viewport;
    return false;
#endif
}
