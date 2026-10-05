#include "d3d_shader_compile.h"

#ifndef _WIN32
#include <vkd3d_shader.h>

#include <atomic>
#include <cstring>
#include <new>
#include <vector>

namespace {

class ShaderBlob final : public ID3DBlob {
    std::atomic<ULONG> references{1};
    std::vector<unsigned char> bytes;
public:
    ShaderBlob(const void *data, size_t size) : bytes(size)
    {
        if (size) std::memcpy(bytes.data(), data, size);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(ID3DBlob)) return E_NOINTERFACE;
        *out = static_cast<ID3DBlob *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG count = --references;
        if (!count) delete this;
        return count;
    }
    void *STDMETHODCALLTYPE GetBufferPointer() override { return bytes.data(); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return bytes.size(); }
};

} // namespace

HRESULT recompCompileShader(const void *source, SIZE_T size, const char *name,
    const D3D_SHADER_MACRO *macros, ID3DInclude *include, const char *entry,
    const char *profile, UINT flags, UINT effect_flags,
    ID3DBlob **code, ID3DBlob **errors)
{
    if (code) *code = nullptr;
    if (errors) *errors = nullptr;
    if (!source || !entry || !profile || !code || include || effect_flags ||
        (flags != 0 && flags != D3DCOMPILE_OPTIMIZATION_LEVEL3)) return E_INVALIDARG;
    vkd3d_shader_code output{};
    char *messages = nullptr;
    HRESULT result = E_FAIL;
    try {
        std::vector<vkd3d_shader_macro> defines;
        for (auto *macro = macros; macro && macro->Name; ++macro) {
            defines.push_back({macro->Name, macro->Definition});
        }
        vkd3d_shader_preprocess_info preprocess{};
        preprocess.type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;
        preprocess.macros = defines.data();
        preprocess.macro_count = static_cast<unsigned>(defines.size());
        vkd3d_shader_hlsl_source_info hlsl{};
        hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
        hlsl.next = &preprocess;
        hlsl.entry_point = entry;
        hlsl.profile = profile;
        vkd3d_shader_compile_info info{};
        info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
        info.next = &hlsl;
        info.source = {source, size};
        info.source_type = VKD3D_SHADER_SOURCE_HLSL;
        info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
        info.log_level = VKD3D_SHADER_LOG_WARNING;
        info.source_name = name;
        const int status = vkd3d_shader_compile(&info, &output, &messages);
        if (errors && messages) *errors = new ShaderBlob(messages, std::strlen(messages) + 1);
        if (status >= 0) {
            *code = new ShaderBlob(output.code, output.size);
            result = S_OK;
        }
    } catch (const std::bad_alloc &) {
        result = E_OUTOFMEMORY;
    }
    vkd3d_shader_free_shader_code(&output);
    vkd3d_shader_free_messages(messages);
    return result;
}
#endif
