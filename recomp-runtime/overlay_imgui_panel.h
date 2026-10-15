#ifndef DOAXBV_RECOMP_OVERLAY_IMGUI_PANEL_H
#define DOAXBV_RECOMP_OVERLAY_IMGUI_PANEL_H

#include "overlay_text.h"

/* The overlay's look and layout in Dear ImGui, shared by every presenter's
   drawer: call the style once after creating the context, and the build
   between ImGui::NewFrame and ImGui::Render. */
void recomp_overlay_imgui_style(void);
void recomp_overlay_imgui_build(const RecompOverlayText &text, float width, float height);

#endif
