#include "d3d_creation_model.h"
#include <stddef.h>

void recomp_d3d_creation_reset(RecompD3dCreationModel *model)
{
    if (model != NULL) {
        *model = (RecompD3dCreationModel){0};
    }
}

static bool presentation_matches_observed_mode(
    const RecompD3dPresentationParameters *presentation)
{
    return presentation != NULL &&
        presentation->back_buffer_width == 0x000002d0u &&
        presentation->back_buffer_height == 0x000001e0u &&
        presentation->back_buffer_format == 0x00000012u &&
        presentation->back_buffer_count == 0x00000001u &&
        presentation->swap_effect == 0x00000001u &&
        presentation->device_window == 0u &&
        presentation->windowed == 0u &&
        presentation->enable_auto_depth_stencil == 1u &&
        presentation->auto_depth_stencil_format == 0x0000002eu &&
        presentation->full_screen_refresh_rate == 0u;
}

static bool presentation_matches_observed_base(
    const RecompD3dPresentationParameters *presentation)
{
    return presentation_matches_observed_mode(presentation) &&
        presentation->multi_sample_type == 0x00000011u &&
        presentation->buffer_surfaces[0] == 0x00a23ab0u &&
        presentation->buffer_surfaces[1] == 0x00a23ac8u &&
        presentation->buffer_surfaces[2] == 0u &&
        presentation->depth_stencil_surface == 0x00a23ae0u;
}

bool recomp_d3d_create_request_supported(
    const RecompD3dCreateRequest *request)
{
    return request != NULL &&
        request->adapter == 0u &&
        request->device_type == 1u &&
        request->focus_window == 0u &&
        request->behavior_flags == 0x00000040u &&
        presentation_matches_observed_base(&request->presentation) &&
        (request->presentation.flags == 0u ||
         request->presentation.flags == 0x00000010u) &&
        request->presentation.full_screen_presentation_interval ==
            0x80000001u;
}

bool recomp_d3d_reset_request_supported(
    const RecompD3dPresentationParameters *presentation)
{
    return presentation_matches_observed_mode(presentation) &&
        (presentation->multi_sample_type == 0x00000011u ||
         presentation->multi_sample_type == 0x00002021u) &&
        presentation->buffer_surfaces[0] != 0u &&
        presentation->buffer_surfaces[1] != 0u &&
        presentation->buffer_surfaces[2] == 0u &&
        presentation->depth_stencil_surface != 0u &&
        presentation->flags == 0x00000010u &&
        (presentation->full_screen_presentation_interval == 1u ||
         presentation->full_screen_presentation_interval == 0x80000001u);
}

static uint32_t count_back_buffer_surfaces(
    const RecompD3dPresentationParameters *presentation)
{
    uint32_t count = 0u;

    while (count < 3u && presentation->buffer_surfaces[count] != 0u) {
        ++count;
    }
    return count;
}

uint32_t recomp_d3d_create_device(
    RecompD3dCreationModel *model,
    const RecompD3dCreateRequest *request,
    const RecompD3dCreateResources *resources,
    uint32_t *output_device)
{
    if (output_device != NULL) {
        *output_device = 0u;
    }
    if (model == NULL || output_device == NULL ||
        !recomp_d3d_create_request_supported(request)) {
        return RECOMP_D3D_INVALID_CALL;
    }
    if (model->device.created) {
        return RECOMP_D3D_INVALID_CALL;
    }
    if (resources == NULL || resources->context == 0u ||
        resources->push_buffer == 0u) {
        return RECOMP_D3D_OUT_OF_MEMORY;
    }

    model->device = (RecompD3dDeviceState){
        .created = true,
        .framebuffer_scale = {1.0f, 1.0f},
        .address = RECOMP_D3D_DEVICE_ADDRESS,
        .width = request->presentation.back_buffer_width,
        .height = request->presentation.back_buffer_height,
        .format = request->presentation.back_buffer_format,
        .depth_stencil_format =
            request->presentation.auto_depth_stencil_format,
        .presenter_config = {
            .width = request->presentation.back_buffer_width,
            .height = request->presentation.back_buffer_height,
            .color_format =
                RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
            .depth_format = RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8,
        },
        .flags = 3u | (request->behavior_flags & 0x10u),
        .context = resources->context,
        .push_buffer_current = resources->push_buffer +
            RECOMP_D3D_PUSH_BUFFER_INITIAL_OFFSET,
        .push_buffer_limit = resources->push_buffer +
            RECOMP_D3D_PUSH_BUFFER_LIMIT_OFFSET,
        .push_buffer_base = resources->push_buffer,
        .push_buffer_end = resources->push_buffer +
            RECOMP_D3D_PUSH_BUFFER_SIZE,
        .channel_base = 0xfd800000u,
        .hardware_base = 0xfd000000u,
        .buffer_surfaces = {
            request->presentation.buffer_surfaces[0],
            request->presentation.buffer_surfaces[1],
            request->presentation.buffer_surfaces[2],
        },
        .depth_stencil_surface =
            request->presentation.depth_stencil_surface,
        .back_buffer_surface_count =
            count_back_buffer_surfaces(&request->presentation),
    };
    *output_device = model->device.address;
    return RECOMP_D3D_OK;
}

uint32_t recomp_d3d_reset_device(
    RecompD3dCreationModel *model,
    const RecompD3dPresentationParameters *presentation)
{
    if (model == NULL || !model->device.created ||
        !recomp_d3d_reset_request_supported(presentation)) {
        return RECOMP_D3D_INVALID_CALL;
    }

    model->device.framebuffer_scale[0] = (float)((presentation->multi_sample_type >> 4u) & 15u);
    model->device.framebuffer_scale[1] = (float)(presentation->multi_sample_type & 15u);
    model->device.width = presentation->back_buffer_width;
    model->device.height = presentation->back_buffer_height;
    model->device.format = presentation->back_buffer_format;
    model->device.depth_stencil_format =
        presentation->auto_depth_stencil_format;
    model->device.presenter_config.width = presentation->back_buffer_width;
    model->device.presenter_config.height = presentation->back_buffer_height;
    model->device.flags &= ~0x00004000u;
    model->device.buffer_surfaces[0] = presentation->buffer_surfaces[0];
    model->device.buffer_surfaces[1] = presentation->buffer_surfaces[1];
    model->device.buffer_surfaces[2] = presentation->buffer_surfaces[2];
    model->device.depth_stencil_surface =
        presentation->depth_stencil_surface;
    model->device.back_buffer_surface_count =
        count_back_buffer_surfaces(presentation);
    return RECOMP_D3D_OK;
}

bool recomp_d3d_persist_display(
    const RecompD3dCreationModel *model,
    uint32_t *saved_surface)
{
    if (model == NULL || saved_surface == NULL || !model->device.created ||
        model->device.back_buffer_surface_count < 2u ||
        model->device.buffer_surfaces[1] == 0u) {
        return false;
    }

    *saved_surface = model->device.buffer_surfaces[1];
    return true;
}

bool recomp_d3d_make_push_space(
    RecompD3dCreationModel *model,
    uint32_t push_buffer_current,
    uint32_t requested_bytes,
    uint32_t reservation_bytes,
    RecompD3dPushSpace *space)
{
    if (space != NULL) {
        *space = (RecompD3dPushSpace){0};
    }
    if (model == NULL || space == NULL || !model->device.created ||
        push_buffer_current < model->device.push_buffer_base ||
        push_buffer_current >= model->device.push_buffer_end ||
        requested_bytes == 0u || reservation_bytes < requested_bytes ||
        reservation_bytes <= 0x204u ||
        reservation_bytes > RECOMP_D3D_PUSH_BUFFER_SIZE) {
        return false;
    }

    *space = (RecompD3dPushSpace){
        .current = model->device.push_buffer_base,
        .limit = model->device.push_buffer_base +
            reservation_bytes - 0x204u,
        .wrap_offset = push_buffer_current - model->device.push_buffer_base,
        .jump = (model->device.push_buffer_base & 0x0fffffffu) + 1u,
    };
    model->device.push_buffer_current = space->current;
    model->device.push_buffer_limit = space->limit;
    return true;
}

bool recomp_d3d_kick_off(
    RecompD3dCreationModel *model,
    uint32_t push_buffer_current,
    uint32_t alternate_command_state,
    uint32_t flags,
    RecompD3dKickOffState *state)
{
    uint32_t command_state;

    if (state != NULL) {
        *state = (RecompD3dKickOffState){0};
    }
    if (model == NULL || state == NULL || !model->device.created ||
        push_buffer_current < model->device.push_buffer_base ||
        push_buffer_current >= model->device.push_buffer_end) {
        return false;
    }

    command_state = (flags & 0x00000004u) != 0u
        ? alternate_command_state
        : push_buffer_current;
    if (command_state < model->device.push_buffer_base ||
        command_state >= model->device.push_buffer_end) {
        return false;
    }

    *state = (RecompD3dKickOffState){
        .restore_channel_state = (flags & 0x00002000u) != 0u,
        .command_state = command_state,
        .dma_put = ((flags & 0x00002000u) != 0u
            ? push_buffer_current
            : command_state) & 0x0fffffffu,
        .flags = flags | 0x00002000u,
    };
    model->device.push_buffer_current = push_buffer_current;
    model->device.flags = state->flags;
    return true;
}
