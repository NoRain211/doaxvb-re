#ifndef DOAXBV_RECOMP_OVERLAY_IMGUI_PANEL_H
#define DOAXBV_RECOMP_OVERLAY_IMGUI_PANEL_H

#include "overlay_text.h"
#include "overlay_screen.h"

#include <stdint.h>

/* The overlay's look and layout in Dear ImGui, shared by every presenter's
   drawer: call the style once after creating the context, with the height
   of the target it will draw to, and the build, with the text and the
   screen's quads (overlay_screen.h), between ImGui::NewFrame and ImGui::Render. */
void recomp_overlay_imgui_style(float target_height);
void recomp_overlay_imgui_build(const RecompOverlayText &text, const RecompOverlayQuad *screen,
    uint32_t screen_count, float width, float height);

#endif
