#ifndef DOAXBV_GAME_ACTOR_H
#define DOAXBV_GAME_ACTOR_H

#include "runtime.h"

typedef struct RecompGameClipAdvance {
    float cursor;
    int32_t frame;
} RecompGameClipAdvance;

RecompGameClipAdvance recomp_game_clip_advance(float cursor, double increment,
    float epsilon, unsigned steps_per_tick);
/* The authored loop samples a pose before it advances the clip, one tick
   behind. Display substep k of steps samples between the tick pass's sample
   and the cursor that tick reached; without a valid tick sample, or across a
   loop, it uses fallback. */
float recomp_game_pose_display_time(float tick_sample, float tick_cursor,
    unsigned substep, unsigned steps, bool valid, float fallback);
RecompFunction recomp_game_actor_lookup_manual(uint32_t address);

#endif
