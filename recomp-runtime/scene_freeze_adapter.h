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
/* Stops drawing the 2D overlay, frozen or not: text, menus, a match's score
   and portraits, logos. */
void recomp_scene_freeze_set_overlay_hidden(bool hidden);
bool recomp_scene_freeze_overlay_hidden(void);
/* True while the scene is frozen and not stepping a frame. */
bool recomp_scene_freeze_holding(void);
/* Lets a frozen scene run one more frame, then holds it again. */
void recomp_scene_freeze_step(void);
/* While `function` is not zero, only the task running that function takes
   its turns, frozen or not; every other task, drawing ones too, waits. Zero
   hands the scene back to the freeze. */
void recomp_scene_freeze_run_only(uint32_t function);
/* Called once a frame while the scene is frozen, from the scheduler, since
   the game's own input polling is frozen with it. */
typedef void (*RecompSceneFreezeFrameHook)(uint32_t frame);
void recomp_scene_freeze_set_frame_hook(RecompSceneFreezeFrameHook hook);
/* Called once a frame whether frozen or not, from the scheduler. */
void recomp_scene_freeze_set_tick_hook(RecompSceneFreezeFrameHook hook);
/* The fiber adapter's switch hook (recomp_fiber_set_switch_hook). */
bool recomp_scene_freeze_switch(uint32_t outgoing_handle, uint32_t target_handle);

#ifdef __cplusplus
}
#endif

#endif
