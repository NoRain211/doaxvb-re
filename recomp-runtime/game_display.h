#ifndef DOAXBV_GAME_DISPLAY_H
#define DOAXBV_GAME_DISPLAY_H
#include <stdbool.h>
#include <stdint.h>
void recomp_game_display_commit(void);
void recomp_game_display_uv(uint32_t resource);
void recomp_game_display_apply(float alpha);
void recomp_game_display_trace(bool tick);
bool recomp_game_display_jump(const float before[3], const float after[3], float limit);
float recomp_game_display_lerp(float before, float after, float alpha);
uint32_t recomp_game_display_angle(uint32_t before, uint32_t after, float alpha);
#endif
