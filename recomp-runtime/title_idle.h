#ifndef DOAXBV_RECOMP_TITLE_IDLE_H
#define DOAXBV_RECOMP_TITLE_IDLE_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the game does when the title screen is left idle; see title_idle.c.
   Once per presented frame. */
void recomp_title_idle_frame(void);
RecompFunction recomp_title_idle_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
