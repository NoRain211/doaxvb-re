#ifndef DOAXBV_RECOMP_CPU_EXHIBITION_H
#define DOAXBV_RECOMP_CPU_EXHIBITION_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Keeps the game in Exhibition matches played by the CPU on every side. */
void recomp_cpu_exhibition_frame(void);
#ifdef RECOMP_FULL_PROGRAM
/* Seam: replaces the routine the logos leave through; see cpu_exhibition.c. */
RecompFunction recomp_cpu_exhibition_lookup_manual(uint32_t guest_address);
#endif

#ifdef __cplusplus
}
#endif

#endif
