#ifndef DOAXBV_D3D_DRAW_SHADER_H
#define DOAXBV_D3D_DRAW_SHADER_H

#include "d3d_draw_model.h"

#include <cstddef>

/* HLSL for the D3D11 presenter's fixed-function draws: the vertex and pixel
   shader for one decoded vertex layout, written NUL-terminated to out. */
void recomp_d3d_draw_shader_source(const RecompD3dVertexLayout &layout, char *out, size_t out_size);

#endif
