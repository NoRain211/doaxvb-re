#ifndef DOAXBV_D3D_GLSL_SHADER_H
#define DOAXBV_D3D_GLSL_SHADER_H

#include <cstdint>
#include <string>

#include "d3d_draw_model.h"

/* Vulkan-flavoured GLSL 450 (descriptor sets as SDL GPU expects). The vertex
   shader is empty when a vertex program does not translate. */
std::string recomp_glsl_vertex_shader(const RecompD3dVertexLayout &layout, uint32_t fvf,
                                      uint32_t program_count, const uint32_t (*program_tokens)[4]);
std::string recomp_glsl_fragment_shader(const RecompD3dVertexLayout &layout, uint32_t fvf,
                                        uint32_t program_count);

#endif
