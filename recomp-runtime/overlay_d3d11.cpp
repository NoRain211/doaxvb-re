#include "overlay_d3d11.h"
#include "overlay_imgui_panel.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

#include <chrono>
#include <cstdio>

namespace {

bool ready;
bool failed;
std::chrono::steady_clock::time_point last_frame;

bool start(ID3D11Device *device, ID3D11DeviceContext *context)
{
    if (ready) return true;
    if (failed) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    recomp_overlay_imgui_style();
    if (!ImGui_ImplDX11_Init(device, context)) {
        std::fprintf(stderr, "[overlay] ImGui D3D11 backend failed to start\n");
        ImGui::DestroyContext();
        failed = true;
        return false;
    }
    ready = true;
    last_frame = std::chrono::steady_clock::now();
    return true;
}

}  // namespace

void recomp_overlay_d3d11_draw(ID3D11Device *device, ID3D11DeviceContext *context,
    ID3D11RenderTargetView *target, UINT width, UINT height, const RecompOverlayText *text)
{
    if (device == nullptr || context == nullptr || target == nullptr ||
        width == 0u || height == 0u || recomp_overlay_empty(text)) {
        return;
    }
    if (!start(device, context)) return;

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    const auto now = std::chrono::steady_clock::now();
    const float elapsed = std::chrono::duration<float>(now - last_frame).count();
    io.DeltaTime = elapsed > 0.0f ? elapsed : 1.0f / 60.0f;
    last_frame = now;

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    recomp_overlay_imgui_build(*text, io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Render();
    ImDrawData *draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr || draw_data->CmdListsCount == 0) return;
    // The backend restores the context state it changes, but not the target.
    context->OMSetRenderTargets(1u, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(draw_data);
    context->OMSetRenderTargets(0u, nullptr, nullptr);
}

void recomp_overlay_d3d11_shutdown(void)
{
    if (!ready) return;
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    ready = false;
}
