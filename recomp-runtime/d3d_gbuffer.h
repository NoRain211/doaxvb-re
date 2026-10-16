#ifndef DOAXBV_D3D_GBUFFER_H
#define DOAXBV_D3D_GBUFFER_H

namespace {

bool gbufferRequested()
{
    const char *value = std::getenv("RECOMP_D3D_GBUFFER");
    return value && std::strcmp(value, "1") == 0;
}

struct GBuffer {
    bool enabled = false;
    unsigned debug_view = 0; // 1 normal, 2 depth
    unsigned cycles_seen = 0; // recomp_tuning.gbuffer_cycles already applied
    ID3D11Texture2D *normal = nullptr;
    ID3D11RenderTargetView *target = nullptr;
    ID3D11ShaderResourceView *sample = nullptr;
    // Consumer views at internal resolution: packed world normal and raw [0,1] depth.
    ID3D11RenderTargetView *resolved_targets[2]{};
    ID3D11ShaderResourceView *resolved[2]{};
    ID3D11VertexShader *vertex = nullptr;
    ID3D11PixelShader *resolve = nullptr, *debug[2]{};

    void release()
    {
        if (normal) normal->Release();
        if (target) target->Release();
        if (sample) sample->Release();
        for (auto view : resolved_targets) if (view) view->Release();
        for (auto view : resolved) if (view) view->Release();
        if (vertex) vertex->Release();
        if (resolve) resolve->Release();
        for (auto shader : debug) if (shader) shader->Release();
        *this = {};
    }

    bool create(ID3D11Device *device, unsigned width, unsigned height, unsigned samples)
    {
        if (!enabled) return true;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        desc.SampleDesc.Count = samples;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT result = device->CreateTexture2D(&desc, nullptr, &normal);
        if (SUCCEEDED(result)) result = device->CreateRenderTargetView(normal, nullptr, &target);
        if (SUCCEEDED(result)) result = device->CreateShaderResourceView(normal, nullptr, &sample);
        desc.SampleDesc.Count = 1;
        for (unsigned i = 0; i < 2 && SUCCEEDED(result); ++i) {
            desc.Format = i ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
            ID3D11Texture2D *texture = nullptr;
            result = device->CreateTexture2D(&desc, nullptr, &texture);
            if (SUCCEEDED(result)) result = device->CreateRenderTargetView(texture, nullptr, &resolved_targets[i]);
            if (SUCCEEDED(result)) result = device->CreateShaderResourceView(texture, nullptr, &resolved[i]);
            if (texture) texture->Release();
        }
        const std::string source = "#define SAMPLES " + std::to_string(samples) + R"(
#if SAMPLES > 1
Texture2DMS<float4, SAMPLES> normals : register(t0);
Texture2DMS<float, SAMPLES> depths : register(t1);
#else
Texture2D<float4> normals : register(t0);
Texture2D<float> depths : register(t1);
#endif
float4 vs(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position {
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2,-2) + float2(-1,1), 0, 1);
}
struct Pair { float4 normal : SV_Target0; float depth : SV_Target1; };
Pair ps(float4 p : SV_Position) {
    int2 pixel = int2(p.xy);
    Pair result;
#if SAMPLES > 1
    result.depth = depths.Load(pixel, 0);
    result.normal = normals.Load(pixel, 0);
    [unroll] for (int i = 1; i < SAMPLES; ++i) {
        float z = depths.Load(pixel, i);
        if (z < result.depth) { result.depth = z; result.normal = normals.Load(pixel, i); }
    }
#else
    result.depth = depths.Load(int3(pixel, 0));
    result.normal = normals.Load(int3(pixel, 0));
#endif
    return result;
}
)";
        ID3DBlob *blob = nullptr, *errors = nullptr;
        if (SUCCEEDED(result)) result = D3DCompile(source.c_str(), source.size(), nullptr,
            nullptr, nullptr, "vs", "vs_4_0", 0, 0, &blob, &errors);
        if (SUCCEEDED(result)) result = device->CreateVertexShader(blob->GetBufferPointer(),
            blob->GetBufferSize(), nullptr, &vertex);
        if (blob) { blob->Release(); blob = nullptr; }
        if (errors) {
            if (FAILED(result)) std::fprintf(stderr, "recomp gbuffer: %s\n", static_cast<const char *>(errors->GetBufferPointer()));
            errors->Release(); errors = nullptr;
        }
        if (SUCCEEDED(result)) result = D3DCompile(source.c_str(), source.size(), nullptr,
            nullptr, nullptr, "ps", "ps_4_0", 0, 0, &blob, &errors);
        if (SUCCEEDED(result)) result = device->CreatePixelShader(blob->GetBufferPointer(),
            blob->GetBufferSize(), nullptr, &resolve);
        if (blob) { blob->Release(); blob = nullptr; }
        if (errors) {
            if (FAILED(result)) std::fprintf(stderr, "recomp gbuffer: %s\n", static_cast<const char *>(errors->GetBufferPointer()));
            errors->Release(); errors = nullptr;
        }
        // Texture loads keep the debug output independent of game sampler/gamma/SMAA state.
        static const char debug_source[] = R"(
Texture2D<float4> normal : register(t0);
Texture2D<float> depth : register(t1);
int3 pixelAt(float2 uv) {
    uint w, h; normal.GetDimensions(w, h);
    return int3(min(uint2(uv * float2(w,h)), uint2(w-1,h-1)), 0);
}
float4 normalView(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 n = normal.Load(pixelAt(uv));
    return float4(n.a > 0 ? n.rgb : 0, 1);
}
float4 depthView(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float z = depth.Load(pixelAt(uv));
    // Logarithmic display of raw device depth; this is not camera-linear depth.
    float d = z < 1 ? log2(1 + 1024 * (1-z)) / log2(1025.0) : 0;
    return float4(d,d,d,1);
}
)";
        for (unsigned i = 0; i < 2 && SUCCEEDED(result); ++i) {
            result = D3DCompile(debug_source, sizeof debug_source - 1, nullptr, nullptr,
                nullptr, i ? "depthView" : "normalView", "ps_4_0", 0, 0, &blob, &errors);
            if (SUCCEEDED(result)) result = device->CreatePixelShader(blob->GetBufferPointer(),
                blob->GetBufferSize(), nullptr, &debug[i]);
            if (blob) { blob->Release(); blob = nullptr; }
            if (errors) { errors->Release(); errors = nullptr; }
        }
        if (FAILED(result)) {
            std::fprintf(stderr, "recomp gbuffer: creation failed hr=0x%08lX; disabled\n",
                static_cast<unsigned long>(result));
            release();
            return false;
        }
        std::fprintf(stderr, "recomp gbuffer: %ux%u samples=%u world normals + device depth\n",
            width, height, samples);
        return true;
    }

    void resolveFrame(ID3D11DeviceContext *context, ID3D11ShaderResourceView *depth)
    {
        context->ClearState(); // Unbind the DSV and MRT before reading their resources.
        D3D11_TEXTURE2D_DESC desc{};
        normal->GetDesc(&desc);
        const D3D11_VIEWPORT viewport = {0, 0, float(desc.Width), float(desc.Height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(2, resolved_targets, nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex, nullptr, 0);
        context->PSSetShader(resolve, nullptr, 0);
        ID3D11ShaderResourceView *views[] = {sample, depth};
        context->PSSetShaderResources(0, 2, views);
        context->Draw(3, 0);
        context->ClearState();
    }

    void show(ID3D11DeviceContext *context, ID3D11RenderTargetView *output)
    {
        context->ClearState();
        ID3D11Resource *resource = nullptr;
        output->GetResource(&resource);
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D *>(resource)->GetDesc(&desc);
        resource->Release();
        const D3D11_VIEWPORT viewport = {0, 0, float(desc.Width), float(desc.Height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, &output, nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex, nullptr, 0);
        context->PSSetShader(debug[debug_view - 1], nullptr, 0);
        context->PSSetShaderResources(0, 2, resolved);
        context->Draw(3, 0);
        context->ClearState();
    }
};

void gbufferShaderSource(std::string &source, const RecompD3dVertexLayout &layout, bool program)
{
    const bool normals = !program && !layout.pretransformed &&
        layout.normal_offset != RECOMP_D3D_FVF_ABSENT;
    if (normals) {
        source.insert(source.find("struct VSOut {") + std::strlen("struct VSOut {"),
            "\n    float3 gbuffer_normal : TEXCOORD13;\n");
        std::string vertex = "    float3 gn = mul(float4(input.normal,0),directional_normals[0]).xyz;\n";
        if (layout.blend_weight_count) {
            vertex += "    if (blend_flags.x > 0.5f) { gn=0; float remainder=1;\n";
            for (unsigned i = 0; i < layout.blend_weight_count; ++i) {
                const auto n = std::to_string(i);
                vertex += "gn += input.weights["+n+"] * mul(float4(input.normal,0),directional_normals["+n+"]).xyz;\n"
                    "remainder -= input.weights["+n+"];\n";
            }
            vertex += "gn += remainder * mul(float4(input.normal,0),directional_normals["+
                std::to_string(layout.blend_weight_count)+"]).xyz; }\n";
        }
        vertex += "    output.gbuffer_normal = gn;\n";
        source.insert(source.find("    return output;"), vertex);
    }
    const auto pixel = source.find("float4 ps_main(");
    const auto signature_end = source.find(") : SV_TARGET", pixel);
    source.insert(signature_end, ", out float4 normal_output : SV_Target1");
    const auto body = source.find('{', signature_end);
    source.insert(body + 1, normals ? R"(
    float n2 = dot(input.gbuffer_normal, input.gbuffer_normal);
    normal_output = n2 > 1e-12 && isfinite(n2)
        ? float4(input.gbuffer_normal * rsqrt(n2) * 0.5 + 0.5, 1) : 0;
)" : "\n    normal_output = 0;\n");
}

} // namespace
#endif
