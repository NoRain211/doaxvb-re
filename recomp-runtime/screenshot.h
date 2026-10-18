#ifndef DOAXBV_RECOMP_SCREENSHOT_H
#define DOAXBV_RECOMP_SCREENSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Screenshots of the game's own frame, without the runtime's overlay. The
   game side asks for one; a presenter, on whatever thread, takes the request
   when it presents and saves the frame it rendered. */
void recomp_screenshot_request(void);
bool recomp_screenshot_take_request(void);

/* Writes BGRA rows (top row first, `pitch` bytes apart) as an RGB PNG.
   Uncompressed deflate keeps it dependency-free; false on any failure. */
bool recomp_screenshot_write_png(const char *path, const uint8_t *bgra,
    uint32_t width, uint32_t height, uint32_t pitch);

/* A new file path under RECOMP_SCREENSHOT_DIR, or Pictures/DOAXBV in the
   user's home, creating the folder. False when no folder can be made. */
bool recomp_screenshot_path(char *path, size_t size);

/* Saves a frame to a new path and reports where; presenters call it after
   taking a request. */
void recomp_screenshot_save(const uint8_t *bgra, uint32_t width,
    uint32_t height, uint32_t pitch);

#ifdef __cplusplus
}
#endif

#endif
