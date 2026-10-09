#include "d3d_presenter_memory_test.h"

#include <stdlib.h>

struct RecompD3dPresenter {
    RecompD3dPresenterMemorySnapshot snapshot;
    RecompD3dPresenterError error;
};

static RecompD3dPresenter *active_presenter;

RecompD3dPresenterError recomp_d3d_presenter_create(
    const RecompD3dPresenterConfig *config,
    RecompD3dPresenter **presenter)
{
    RecompD3dPresenter *created;

    if (config == NULL || presenter == NULL) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    if (*presenter != NULL || active_presenter != NULL) {
        return RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED;
    }
    if (config->width == 0u || config->height == 0u ||
        config->color_format !=
            RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM ||
        config->depth_format != RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }

    created = (RecompD3dPresenter *)calloc(1u, sizeof *created);
    if (created == NULL) {
        return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    }
    created->snapshot.config = *config;
    active_presenter = created;
    *presenter = created;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError recomp_d3d_presenter_submit(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterCommand *command)
{
    RecompD3dPresenterMemorySnapshot *snapshot;

    if (presenter == NULL || presenter != active_presenter) {
        return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    }
    if (presenter->error != RECOMP_D3D_PRESENTER_OK) return presenter->error;
    if (command == NULL) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    snapshot = &presenter->snapshot;
    if (snapshot->command_count >=
        RECOMP_D3D_PRESENTER_MEMORY_COMMAND_CAPACITY) {
        return RECOMP_D3D_PRESENTER_COMMAND_LIMIT;
    }

    switch (command->type) {
    case RECOMP_D3D_PRESENTER_COMMAND_CLEAR:
        ++snapshot->clear_count;
        break;
    case RECOMP_D3D_PRESENTER_COMMAND_PRESENT:
        ++snapshot->present_count;
        break;
    case RECOMP_D3D_PRESENTER_COMMAND_GAMMA:
        break;
    case RECOMP_D3D_PRESENTER_COMMAND_DRAW:
        ++snapshot->draw_count;
        break;
    default:
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    snapshot->commands[snapshot->command_count++] = *command;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError recomp_d3d_presenter_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size)
{
    if (presenter == NULL || presenter != active_presenter) {
        return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    }
    if (base >= 0x80000000u && base < 0x84000000u) base -= 0x80000000u;
    if (size == 0u || (uint64_t)base + size > UINT64_C(0x100000000)) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    /* This backend retains commands, not uploaded pixels. */
    return presenter->error;
}

RecompD3dPresenterError recomp_d3d_presenter_destroy(
    RecompD3dPresenter **presenter)
{
    if (presenter == NULL) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    if (*presenter == NULL || *presenter != active_presenter) {
        return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    }

    free(*presenter);
    *presenter = NULL;
    active_presenter = NULL;
    return RECOMP_D3D_PRESENTER_OK;
}

void recomp_d3d_presenter_set_immediate_present(bool enabled)
{
    /* The in-memory presenter never blocks; there is no pacing to remove. */
    (void)enabled;
}

bool recomp_d3d_presenter_split_enabled(void)
{
    return false;
}

void recomp_d3d_presenter_report_draw_textures(void)
{
    /* The in-memory presenter uploads no textures, so it has none to report. */
}

void recomp_d3d_presenter_memory_set_error(RecompD3dPresenterError error)
{
    if (active_presenter != NULL) active_presenter->error = error;
}

bool recomp_d3d_presenter_memory_snapshot(
    RecompD3dPresenterMemorySnapshot *snapshot)
{
    if (snapshot == NULL || active_presenter == NULL) {
        return false;
    }
    *snapshot = active_presenter->snapshot;
    return true;
}
