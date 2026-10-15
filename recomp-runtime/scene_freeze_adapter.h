#ifndef DOAXBV_RECOMP_SCENE_FREEZE_ADAPTER_H
#define DOAXBV_RECOMP_SCENE_FREEZE_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Freezes and thaws the running scene; see scene_freeze_adapter.c. */
void recomp_scene_freeze_set(bool frozen);
bool recomp_scene_freeze_active(void);
/* While frozen, also stop drawing the 2D overlay: text, menus, logos. */
void recomp_scene_freeze_set_overlay_hidden(bool hidden);
bool recomp_scene_freeze_overlay_hidden(void);
/* The fiber adapter's switch hook (recomp_fiber_set_switch_hook). */
bool recomp_scene_freeze_switch(uint32_t outgoing_handle, uint32_t target_handle);

#ifdef __cplusplus
}
#endif

#endif
