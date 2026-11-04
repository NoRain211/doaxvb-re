#ifndef DOAXBV_RECOMP_OVERLAY_IMGUI_PANEL_H
#define DOAXBV_RECOMP_OVERLAY_IMGUI_PANEL_H

#include "overlay_text.h"

#include <stdint.h>

/* The overlay's look and layout in Dear ImGui, shared by every presenter's
   drawer: call the style once after creating the context, with the height
   of the target it will draw to, and the build between ImGui::NewFrame and
   ImGui::Render. */
void recomp_overlay_imgui_style(float target_height);
/* Takes the latest screen (overlay_screen.h) for the next build; its quad count. */
uint32_t recomp_overlay_imgui_take_screen(void);
void recomp_overlay_imgui_build(const RecompOverlayText &text, float width, float height);

#endif
