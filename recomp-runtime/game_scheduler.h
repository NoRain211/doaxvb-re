#ifndef DOAXBV_GAME_SCHEDULER_H
#define DOAXBV_GAME_SCHEDULER_H

#include "runtime.h"

/* Guest sleep counts remain integer 60 Hz ticks. The remainder belongs to
   one sleep, and must be cleared when a slot is disabled or resumes. */
bool recomp_game_sleep_step(int32_t *ticks, unsigned *subtick, unsigned steps);
void recomp_game_scheduler_reset(unsigned hz);
void recomp_game_scheduler_tick(void);
/* 60 times the substeps per tick of the current pass: 60 on a tick pass. */
unsigned recomp_game_scheduler_hz(void);
/* True on a tick pass, which polls input and runs the authored update. */
bool recomp_game_authored_pass(void);
/* Substeps of the following tick that precede this display pass; 0 on a
   tick pass. A display pass shows substep recomp_game_substep() + 1. */
unsigned recomp_game_substep(void);
/* Starts a game-loop pass. A display pass presents substep + 1 of steps
   of the following tick and is undone before the next tick pass. */
void recomp_game_pass_begin(bool tick, unsigned steps, unsigned substep);
/* Passes started since boot; consecutive passes differ by one. */
uint32_t recomp_game_pass(void);
/* Tick passes started since boot. */
uint32_t recomp_game_tick(void);
uint32_t recomp_game_debug_rng_state(void);
/* Guest address of the CRT rand() state, or 0 before the CRT has one. */
uint32_t recomp_game_rng_address(void);
unsigned recomp_game_requested_hz(void);
bool recomp_game_timing_enabled(void);
void recomp_game_test_clock_begin(bool enabled);
RecompFunction recomp_game_scheduler_lookup_manual(uint32_t address);

#endif
