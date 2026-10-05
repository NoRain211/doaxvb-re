#ifndef DOAXBV_RECOMP_D3D_SHADER_COMPILE_H
#define DOAXBV_RECOMP_D3D_SHADER_COMPILE_H

#include <d3dcompiler.h>

#ifdef _WIN32
inline constexpr auto recompCompileShader = D3DCompile;
#else
HRESULT recompCompileShader(const void *source, SIZE_T size, const char *name,
    const D3D_SHADER_MACRO *macros, ID3DInclude *include, const char *entry,
    const char *profile, UINT flags, UINT effect_flags,
    ID3DBlob **code, ID3DBlob **errors);
#endif

#endif
