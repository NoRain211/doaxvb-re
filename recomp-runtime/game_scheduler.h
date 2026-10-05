#ifndef DOAXBV_GAME_SCHEDULER_H
#define DOAXBV_GAME_SCHEDULER_H

#include "runtime.h"

/* Guest sleep counts remain integer 60 Hz ticks. The remainder belongs to
   one sleep, and must be cleared when a slot is disabled or resumes. */
bool recomp_game_sleep_step(int32_t *ticks, unsigned *subtick, unsigned steps);
void recomp_game_scheduler_reset(unsigned hz);
void recomp_game_scheduler_tick(void);
unsigned recomp_game_scheduler_hz(void);
/* True on the pass that polled input, which starts an authored tick. */
bool recomp_game_authored_pass(void);
/* Once-per-tick work sees the authored step and, on a pass that did not
   poll, the tick's input edges; end restores this pass's values. */
bool recomp_game_tick_scope_begin(void);
void recomp_game_tick_scope_end(bool begun);
/* The scene fiber refreshed its copy of the input records. */
void recomp_game_tick_input_scene_copied(void);
uint32_t recomp_game_debug_rng_state(void);
/* Guest address of the CRT rand() state, or 0 before the CRT has one. */
uint32_t recomp_game_rng_address(void);
unsigned recomp_game_requested_hz(void);
bool recomp_game_timing_enabled(void);
void recomp_game_test_clock_begin(bool enabled);
RecompFunction recomp_game_scheduler_lookup_manual(uint32_t address);

#endif
