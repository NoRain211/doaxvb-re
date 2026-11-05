#ifndef DOAXBV_GAME_ROLLBACK_H
#define DOAXBV_GAME_ROLLBACK_H

#include <stdbool.h>

/* Whole-game display passes interpolate completed ticks and are then
   undone. Commit records the authored state after a tick pass; restore puts
   it back before the next pass: guest RAM, fibers, the heap and the FPU
   context. Guest RAM must have been allocated with write tracking that no
   other snapshot ring already uses; otherwise begin fails and every pass
   stays a tick pass. */
bool recomp_game_rollback_begin(void);
void recomp_game_rollback_commit(void);
void recomp_game_rollback_restore(void);

#endif
