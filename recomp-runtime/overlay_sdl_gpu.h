#ifndef DOAXBV_RECOMP_OVERLAY_SDL_GPU_H
#define DOAXBV_RECOMP_OVERLAY_SDL_GPU_H

#include <SDL3/SDL_gpu.h>

#include "overlay_text.h"
#include "overlay_screen.h"

/* Draws the runtime's overlay text (overlay_text.h) and screen quads
   (overlay_screen.h) with Dear ImGui into a colour target of the window's
   swapchain format, after the game's frame. Created on first use; nothing is
   drawn while both are empty. */
void recomp_overlay_sdl_gpu_draw(SDL_GPUDevice *device, SDL_Window *window,
    SDL_GPUCommandBuffer *command_buffer, SDL_GPUTexture *target,
    Uint32 width, Uint32 height, const RecompOverlayText *text,
    const RecompOverlayQuad *screen, uint32_t screen_count);
void recomp_overlay_sdl_gpu_shutdown(void);

#endif
