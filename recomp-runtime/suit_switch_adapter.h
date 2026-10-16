#ifndef DOAXBV_RECOMP_SUIT_SWITCH_ADAPTER_H
#define DOAXBV_RECOMP_SUIT_SWITCH_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Puts the next (step > 0) or previous swimsuit the character can wear on
   character `actor`, live. True when a switch began. */
bool recomp_suit_switch_step(uint32_t actor, int step);
/* True while a switch is under way. */
bool recomp_suit_switch_busy(void);
/* The swimsuit character `actor` wears, or 0 when she has none. */
uint16_t recomp_suit_switch_current(uint32_t actor);
/* The swimsuit's name in the game's language (English for Japanese), or
   "Swimsuit n". */
void recomp_suit_switch_name(uint16_t suit, char *name, uint32_t size);
/* From the free camera's wrap of the camera build, before the build. */
void recomp_suit_switch_camera_build(void);
/* Once a frame, from the scene freeze's tick, between task turns. */
void recomp_suit_switch_frame(uint32_t frame);

#ifdef __cplusplus
}
#endif

#endif
