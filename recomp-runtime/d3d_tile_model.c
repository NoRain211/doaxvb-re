#include "d3d_tile_model.h"
#include <stddef.h>

bool recomp_d3d_tile_entry_address(
    uint32_t device_address,
    uint32_t index,
    uint32_t *address)
{
    if (address == NULL || device_address == 0u ||
        index >= RECOMP_D3D_TILE_COUNT) {
        return false;
    }

    /* The device global is guest-writable, so a high base could carry the
       array past the end of the address space and wrap back to low memory. */
    if (device_address > UINT32_MAX - RECOMP_D3D_TILE_ARRAY_EXTENT) {
        return false;
    }

    *address = device_address + RECOMP_D3D_TILE_ARRAY_OFFSET +
        index * RECOMP_D3D_TILE_ENTRY_SIZE;
    return true;
}

bool recomp_d3d_tile_range_fits(uint32_t address)
{
    return address <= UINT32_MAX - RECOMP_D3D_TILE_ENTRY_SPAN;
}

void recomp_d3d_set_tile(
    RecompD3dTile *destination,
    const RecompD3dTile *source)
{
    if (source == NULL || source->words[1] == 0u) {
        *destination = (RecompD3dTile){0};
        return;
    }

    *destination = *source;
    if ((source->words[0] & 0x80000000u) == 0u) {
        destination->words[4] = 0u;
        destination->words[5] = 0u;
    }
}
