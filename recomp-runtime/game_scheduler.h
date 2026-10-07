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
/* Hide the authoritative frame; completed snapshots supply all HFR frames. */
bool recomp_game_tick_hidden(void);
/* Fractional-model test seam; runtime display uses completed snapshots. */
unsigned recomp_game_substep(void);
/* Starts a pass; display writes are undone before the following pass. */
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
