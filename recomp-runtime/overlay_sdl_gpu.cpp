#include "overlay_sdl_gpu.h"

#include "imgui.h"
#include "backends/imgui_impl_sdlgpu3.h"

#include <SDL3/SDL_timer.h>

#include <cstdio>

namespace {

bool ready;
bool failed;
SDL_GPUDevice *owner;
Uint64 last_ticks;

/* The panel's text is sized for the target's height, as the game's own is. */
constexpr float kReferenceHeight = 720.0f;
constexpr float kMargin = 18.0f;

bool start(SDL_GPUDevice *device, SDL_Window *window)
{
    if (ready) return true;
    if (failed) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.02f, 0.06f, 0.10f, 0.62f);

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

void build(const RecompOverlayText &text, float width, float height)
{
    const float scale = height / kReferenceHeight;
    ImGui::GetIO().FontGlobalScale = scale > 1.0f ? scale : 1.0f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    if (text.title[0] != '\0' || text.count != 0u) {
        ImGui::SetNextWindowPos(ImVec2(kMargin * scale, kMargin * scale));
        ImGui::Begin("recomp-overlay-panel", nullptr, flags);
        if (text.title[0] != '\0') {
            ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.35f, 1.0f), "%s", text.title);
        }
        for (unsigned i = 0u; i < text.count; ++i) ImGui::TextUnformatted(text.line[i]);
        ImGui::End();
    }
    if (text.hint[0] != '\0') {
        ImGui::SetNextWindowPos(ImVec2(width * 0.5f, height - kMargin * scale), 0, ImVec2(0.5f, 1.0f));
        ImGui::Begin("recomp-overlay-hint", nullptr, flags);
        /* Each line of the hint is centred on its own. */
        const char *line = text.hint;
        while (*line != '\0') {
            const char *end = line;
            while (*end != '\0' && *end != '\n') ++end;
            const float line_width = ImGui::CalcTextSize(line, end).x;
            const float room = ImGui::GetContentRegionAvail().x;
            if (room > line_width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (room - line_width) * 0.5f);
            ImGui::TextUnformatted(line, end);
            line = *end == '\n' ? end + 1 : end;
        }
        ImGui::End();
    }
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
    if (!start(device, window)) return;

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    const Uint64 now = SDL_GetTicks();
    io.DeltaTime = now > last_ticks ? static_cast<float>(now - last_ticks) / 1000.0f : 1.0f / 60.0f;
    last_ticks = now;

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui::NewFrame();
    build(*text, io.DisplaySize.x, io.DisplaySize.y);
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
