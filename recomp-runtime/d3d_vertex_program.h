#ifndef DOAXBV_D3D_VERTEX_PROGRAM_H
#define DOAXBV_D3D_VERTEX_PROGRAM_H

#include <cstdint>
#include <string>

/* Translate straight-line Xbox vertex instructions to native HLSL.
   Unsupported instructions fail closed; no guest execution occurs here. */
bool recomp_d3d_vertex_program_source(
    const uint32_t (*tokens)[4], uint32_t count, std::string &body);

/* Translate straight-line Xbox vertex instructions to native MSL. */
bool recomp_d3d_vertex_program_msl_source(
    const uint32_t (*tokens)[4], uint32_t count, std::string &body);

/* Translate straight-line Xbox vertex instructions to native GLSL (Vulkan). */
bool recomp_d3d_vertex_program_glsl_source(
    const uint32_t (*tokens)[4], uint32_t count, std::string &body);

#endif
