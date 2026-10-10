#include "kernel_abi.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    REG_BINARY = 3u,
    REG_DWORD = 4u,
};

/* XC_VIDEO flags: 60Hz, 480p/720p/1080i and widescreen 0x010000 (nxdk
   hal/video.h). The dashboard's Widescreen choice never also sets letterbox
   0x100000. RECOMP_D3D_WIDESCREEN=0 clears widescreen, the dashboard's Normal
   setting, so the game renders 4:3. The presenter reads the same variable to
   size the window; keep the two in step. */
static uint32_t video_flags(void)
{
    const char *widescreen = getenv("RECOMP_D3D_WIDESCREEN");

    return widescreen != NULL && strcmp(widescreen, "0") == 0
        ? 0x004e0000u
        : 0x004f0000u;
}

uint32_t recomp_kernel_query_nonvolatile_setting(
    uint32_t value_index,
    uint32_t type,
    uint32_t value,
    uint32_t value_length,
    uint32_t result_length)
{
    uint32_t dword_value = value_index == 0x08u ? video_flags() : 0u;
    int dword_backed = value_length == sizeof dword_value;

    if (type != 0u) {
        *recomp_memory_u32(type) = dword_backed ? REG_DWORD : REG_BINARY;
    }
    if (result_length != 0u) {
        *recomp_memory_u32(result_length) =
            dword_backed ? (uint32_t)sizeof dword_value : value_length;
    }
    if (value != 0u && value_length > 0u) {
        recomp_guest_memset(value, 0, value_length);
        if (dword_backed) {
            *recomp_memory_u32(value) = dword_value;
        } else if (value_index == 0xffffu && value_length >= 0x46u) {
            for (uint32_t i = 0u; i < 0x10u; ++i) {
                *recomp_memory_i8(value + 0x36u + i) =
                    (int8_t)(0x11u + i);
            }
        }
    }
    return 0u;
}

static void bridge_ex_query_nonvolatile_setting(void)
{
    uint32_t value_index = kernel_arg(1u);
    uint32_t type = kernel_arg(2u);
    uint32_t value = kernel_arg(3u);
    uint32_t value_length = kernel_arg(4u);
    uint32_t result_length = kernel_arg(5u);

    kernel_return(
        5u,
        recomp_kernel_query_nonvolatile_setting(
            value_index, type, value, value_length, result_length));
}

RecompFunction recomp_kernel_config(uint32_t ordinal)
{
    switch (ordinal) {
    case 24u: return bridge_ex_query_nonvolatile_setting;
    default: return NULL;
    }
}
