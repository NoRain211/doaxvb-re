/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOAXBV_RECOMP_OVERLAY_SCREEN_H
#define DOAXBV_RECOMP_OVERLAY_SCREEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Screens the runner draws over the game, such as the netplay menu:
   rectangles and text in a 640 x 480 space kept 4:3 and centered on the
   window, from doa2u-re's overlay. Coordinates may run past the edges to
   cover a wide window. */
enum { RECOMP_OVERLAY_MAX_QUADS = 8192u };

typedef struct RecompOverlayQuad {
    float x, y, width, height;
    uint32_t argb;
} RecompOverlayQuad;

void recomp_overlay_rect(float x, float y, float width, float height, uint32_t argb);
/* Text in the built-in 5 x 7 font, scale pixels per dot; returns its width.
   Lower case draws as upper case and anything else outside ASCII as '?'. */
float recomp_overlay_text(float x, float y, float scale, uint32_t argb, const char *text);
float recomp_overlay_text_width(const char *text, float scale);
/* The game side hands this frame's shapes over, none to clear the screen,
   and empties the queue; a presenter takes a copy of the latest to draw. */
void recomp_overlay_screen_publish(void);
uint32_t recomp_overlay_screen_take(RecompOverlayQuad *quads, uint32_t capacity);

#ifdef __cplusplus
}
#endif

#endif

