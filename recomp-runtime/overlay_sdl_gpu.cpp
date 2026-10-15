#include "overlay_sdl_gpu.h"
#include "overlay_imgui_panel.h"

#include "imgui.h"
#include "backends/imgui_impl_sdlgpu3.h"

#include <SDL3/SDL_timer.h>

#include <cstdio>

namespace {

bool ready;
bool failed;
SDL_GPUDevice *owner;
Uint64 last_ticks;

bool start(SDL_GPUDevice *device, SDL_Window *window, Uint32 height)
{
    if (ready) return true;
    if (failed) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    recomp_overlay_imgui_style(static_cast<float>(height));

    ImGui_ImplSDLGPU3_InitInfo info{};
    info.Device = device;
    info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    if (!ImGui_ImplSDLGPU3_Init(&info)) {
        std::fprintf(stderr, "[overlay] ImGui SDL GPU backend failed to start\n");
        ImGui::DestroyContext();
        failed = true;
        return false;
    }
    owner = device;
    ready = true;
    last_ticks = SDL_GetTicks();
    return true;
}

}  // namespace

void recomp_overlay_sdl_gpu_draw(SDL_GPUDevice *device, SDL_Window *window,
    SDL_GPUCommandBuffer *command_buffer, SDL_GPUTexture *target,
    Uint32 width, Uint32 height, const RecompOverlayText *text)
{
    if (device == nullptr || window == nullptr || command_buffer == nullptr || target == nullptr ||
        width == 0u || height == 0u || recomp_overlay_empty(text)) {
        return;
    }
    if (!start(device, window, height)) return;

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    const Uint64 now = SDL_GetTicks();
    io.DeltaTime = now > last_ticks ? static_cast<float>(now - last_ticks) / 1000.0f : 1.0f / 60.0f;
    last_ticks = now;

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui::NewFrame();
    recomp_overlay_imgui_build(*text, io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Render();
    ImDrawData *draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr || draw_data->CmdListsCount == 0) return;

    Imgui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);
    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.load_op = SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command_buffer, &color, 1, nullptr);
    if (pass == nullptr) return;
    ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, pass);
    SDL_EndGPURenderPass(pass);
}

void recomp_overlay_sdl_gpu_shutdown(void)
{
    if (!ready) return;
    SDL_WaitForGPUIdle(owner);
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui::DestroyContext();
    ready = false;
}
