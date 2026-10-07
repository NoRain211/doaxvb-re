#ifndef DOAXBV_GAME_SCENE_H
#define DOAXBV_GAME_SCENE_H

#include "runtime.h"

RecompFunction recomp_game_scene_lookup_manual(uint32_t address);
void recomp_game_scene_draw(void);
void recomp_game_scenery_draw(void);
/* The opening movie owns presentation; display passes wait for its end. */
bool recomp_game_startup_movie_playing(void);
/* Places the ball for display substep of steps; advanced is true on the
   display pass that ran the match update a tick ahead. */
void recomp_game_ball_display(bool advanced, unsigned substep, unsigned steps);
void recomp_game_actor_display(bool tick, unsigned substep, unsigned steps);

#endif
