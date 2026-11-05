#ifndef DOAXBV_RECOMP_CPU_EXHIBITION_H
#define DOAXBV_RECOMP_CPU_EXHIBITION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts Exhibition matches played by the CPU on every side, from the title
   screen. They follow one another until START is pressed. */
void recomp_cpu_exhibition_start(void);
/* Once per presented frame. */
void recomp_cpu_exhibition_frame(void);
/* The mode's host state, for SyncTest and rollback snapshots. */
size_t recomp_cpu_exhibition_state_size(void);
void recomp_cpu_exhibition_state_save(void *state);
void recomp_cpu_exhibition_state_restore(const void *state);

#ifdef __cplusplus
}
#endif

#endif
