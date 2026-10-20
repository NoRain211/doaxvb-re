#ifndef DOAXBV_GAME_ACTOR_H
#define DOAXBV_GAME_ACTOR_H

#include "runtime.h"

typedef struct RecompGameClipAdvance {
    float cursor;
    int32_t frame;
} RecompGameClipAdvance;

RecompGameClipAdvance recomp_game_clip_advance(float cursor, double increment,
    float epsilon, unsigned steps_per_tick);
RecompFunction recomp_game_actor_lookup_manual(uint32_t address);

#endif
