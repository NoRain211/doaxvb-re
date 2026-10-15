#include "scene_freeze_adapter.h"
#include "scene_freeze.h"

#include <stdio.h>

#ifdef RECOMP_FULL_PROGRAM
#include "d3d_draw_adapter.h"
#include "d3d_frame_adapter.h"
#include "runtime.h"

/* Scene freeze: stops the game where it is while it goes on drawing, so the
   free camera can look around a still scene.

   The game runs its systems as cooperative tasks on fibers. The task table
   at 0x5F2C38 holds 32-byte records: +0 the state, +4 set once the task has
   yielded, +8 its fiber and +0xC its function. Each frame the scheduler
   fiber records the task it is about to run in 0x5F2E5C and switches to the
   task's fiber, and the task switches back to the scheduler, whose fiber is
   at 0x5F2E58, when its turn is over. Only some tasks draw, and which ones
   depends on the scene: the island map draws from 0x00079D00 and
   0x0008A970, the real-time demo from 0x000B42B0 and 0x0008A970.

   This adapter watches every turn and counts the draws each task issues
   (scene_freeze.c keeps the history). While frozen, a turn of a task that has
   not drawn lately is declined: the switch returns at once and the record is
   left as the task leaves it on a yield, with bit 0x10 of the state clear
   and +4 set. The drawing tasks keep their turns, so the picture stays on
   screen, including the camera the free camera puts in place.

   0x0008A970 draws the 2D overlay (text, portraits, menus and the demo's
   logo) in every scene seen so far. With the overlay hidden, a frozen scene
   declines its turns too, leaving only the 3D scene.

   A step lets every task take its turns until the next frame is presented,
   so a frozen scene can be moved on one frame at a time. */
enum {
    TASK_TABLE = 0x005F2C38u,
    TASK_RECORD_SIZE = 0x20u,
    TASK_COUNT = 17u,
    TASK_STATE = 0x00u,
    TASK_YIELDED = 0x04u,
    TASK_FIBER = 0x08u,
    TASK_FUNCTION = 0x0Cu,
    TASK_STATE_RUNNING = 0x10u,
    TASK_CURRENT = 0x005F2E5Cu,
    OVERLAY_TASK_FUNCTION = 0x0008A970u,
};

static RecompSceneFreezeModel model;
static bool frozen, overlay_hidden;
static uint32_t turn_draws;
/* While the swap counter is below this, a frozen scene runs as usual. */
static uint32_t step_until;
static RecompSceneFreezeFrameHook frame_hook, tick_hook;
static uint32_t hooked_frame, ticked_frame;

void recomp_scene_freeze_set(bool freeze)
{
    if (frozen == freeze) return;
    frozen = freeze;
    fprintf(stderr, "recomp scene freeze: %s", freeze ? "on, drawing tasks" : "off");
    if (freeze) {
        for (uint32_t slot = 0u; slot < TASK_COUNT; ++slot) {
            uint32_t function = *recomp_memory_u32(TASK_TABLE + slot * TASK_RECORD_SIZE + TASK_FUNCTION);
            if (recomp_scene_freeze_keeps(&model, slot, function)) {
                fprintf(stderr, " %08x", function);
            }
        }
    }
    fputc('\n', stderr);
}

bool recomp_scene_freeze_active(void)
{
    return frozen;
}

void recomp_scene_freeze_set_overlay_hidden(bool hidden)
{
    if (overlay_hidden == hidden) return;
    overlay_hidden = hidden;
    fprintf(stderr, "recomp scene freeze: overlay %s\n", hidden ? "hidden" : "shown");
}

bool recomp_scene_freeze_overlay_hidden(void)
{
    return overlay_hidden;
}

void recomp_scene_freeze_set_frame_hook(RecompSceneFreezeFrameHook hook)
{
    frame_hook = hook;
}

void recomp_scene_freeze_set_tick_hook(RecompSceneFreezeFrameHook hook)
{
    tick_hook = hook;
}

void recomp_scene_freeze_step(void)
{
    if (frozen) step_until = recomp_d3d_frame_adapter_swap_counter() + 1u;
}

bool recomp_scene_freeze_switch(uint32_t outgoing_handle, uint32_t target_handle)
{
    uint32_t slot = *recomp_memory_u32(TASK_CURRENT);
    if (slot >= TASK_COUNT) return false;
    uint32_t record = TASK_TABLE + slot * TASK_RECORD_SIZE;
    uint32_t fiber = *recomp_memory_u32(record + TASK_FIBER);
    uint32_t function = *recomp_memory_u32(record + TASK_FUNCTION);
    uint32_t draws = recomp_d3d_draw_adapter_submitted();
    uint32_t frame = recomp_d3d_frame_adapter_swap_counter();
    bool holding = frozen && frame >= step_until;

    if (tick_hook != NULL && ticked_frame != frame) {
        ticked_frame = frame;
        tick_hook(frame);
    }
    if (frozen && frame_hook != NULL && hooked_frame != frame) {
        hooked_frame = frame;
        frame_hook(frame);
    }

    if (outgoing_handle == fiber && target_handle != fiber) {
        /* The turn is over. A frozen scene keeps its history as it stood. */
        if (!holding) recomp_scene_freeze_note_turn(&model, slot, function, draws - turn_draws);
        return false;
    }
    if (target_handle != fiber || outgoing_handle == fiber) return false;
    turn_draws = draws;
    if (!holding) return false;
    if (recomp_scene_freeze_keeps(&model, slot, function) &&
        !(overlay_hidden && function == OVERLAY_TASK_FUNCTION)) return false;
    *recomp_memory_u32(record + TASK_STATE) &= ~TASK_STATE_RUNNING;
    *recomp_memory_u32(record + TASK_YIELDED) = 1u;
    return true;
}
#else
void recomp_scene_freeze_set(bool freeze)
{
    (void)freeze;
}

bool recomp_scene_freeze_active(void)
{
    return false;
}

void recomp_scene_freeze_set_overlay_hidden(bool hidden)
{
    (void)hidden;
}

bool recomp_scene_freeze_overlay_hidden(void)
{
    return false;
}

void recomp_scene_freeze_set_frame_hook(RecompSceneFreezeFrameHook hook)
{
    (void)hook;
}

void recomp_scene_freeze_set_tick_hook(RecompSceneFreezeFrameHook hook)
{
    (void)hook;
}

void recomp_scene_freeze_step(void)
{
}

bool recomp_scene_freeze_switch(uint32_t outgoing_handle, uint32_t target_handle)
{
    (void)outgoing_handle;
    (void)target_handle;
    return false;
}
#endif
