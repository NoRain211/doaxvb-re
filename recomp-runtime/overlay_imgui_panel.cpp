#include "overlay_imgui_panel.h"

#include "imgui.h"

namespace {

/* The panel's text is sized for the target's height, as the game's own is:
   ImGui's 13-pixel font at kReferenceHeight lines. The font is built at that
   size once, so it stays sharp. */
constexpr float kReferenceHeight = 600.0f;
constexpr float kFontPixels = 13.0f;
constexpr float kMargin = 12.0f;
float panel_scale = 1.0f;

}  // namespace

void recomp_overlay_imgui_style(float target_height)
{
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    panel_scale = target_height > kReferenceHeight ? target_height / kReferenceHeight : 1.0f;
    ImFontConfig font{};
    font.SizePixels = kFontPixels * panel_scale;
    io.Fonts->AddFontDefault(&font);
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.02f, 0.06f, 0.10f, 0.62f);
    style.ScaleAllSizes(panel_scale);
}

void recomp_overlay_imgui_build(const RecompOverlayText &text, float width, float height)
{
    const float scale = panel_scale;
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
