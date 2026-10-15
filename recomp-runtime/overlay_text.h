#ifndef DOAXBV_RECOMP_OVERLAY_TEXT_H
#define DOAXBV_RECOMP_OVERLAY_TEXT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Text the runtime shows over the game: a short panel of lines and a hint
   line along the bottom. The game side fills it once a frame; a presenter
   draws whatever it holds when the frame is presented. Plain data, with no
   knowledge of how or where it is drawn. */
enum {
    RECOMP_OVERLAY_LINES = 8,
    RECOMP_OVERLAY_LINE_SIZE = 96,
    RECOMP_OVERLAY_HINT_SIZE = 192,
};

typedef struct RecompOverlayText {
    char title[RECOMP_OVERLAY_LINE_SIZE];
    char line[RECOMP_OVERLAY_LINES][RECOMP_OVERLAY_LINE_SIZE];
    unsigned count;
    char hint[RECOMP_OVERLAY_HINT_SIZE];
} RecompOverlayText;

/* Empties the overlay; an empty overlay draws nothing. */
void recomp_overlay_clear(RecompOverlayText *text);
void recomp_overlay_set_title(RecompOverlayText *text, const char *title);
/* Adds a printf-formatted line; lines past RECOMP_OVERLAY_LINES are dropped
   and long lines are cut. False when the line was dropped. */
bool recomp_overlay_add_line(RecompOverlayText *text, const char *format, ...);
void recomp_overlay_set_hint(RecompOverlayText *text, const char *hint);
bool recomp_overlay_empty(const RecompOverlayText *text);

/* The overlay the presenter draws. */
RecompOverlayText *recomp_overlay_shown(void);

#ifdef __cplusplus
}
#endif

#endif
