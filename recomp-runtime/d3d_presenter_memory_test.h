#ifndef DOAXBV_RECOMP_D3D_PRESENTER_MEMORY_TEST_H
#define DOAXBV_RECOMP_D3D_PRESENTER_MEMORY_TEST_H

#include "d3d_presenter.h"

#include <stdbool.h>
#include <stddef.h>

enum {
    RECOMP_D3D_PRESENTER_MEMORY_COMMAND_CAPACITY = 16u,
};

typedef struct RecompD3dPresenterMemorySnapshot {
    RecompD3dPresenterConfig config;
    size_t command_count;
    size_t clear_count;
    size_t present_count;
    size_t draw_count;
    RecompD3dPresenterCommand
        commands[RECOMP_D3D_PRESENTER_MEMORY_COMMAND_CAPACITY];
} RecompD3dPresenterMemorySnapshot;

void recomp_d3d_presenter_memory_set_error(RecompD3dPresenterError error);

bool recomp_d3d_presenter_memory_snapshot(
    RecompD3dPresenterMemorySnapshot *snapshot);

#endif
