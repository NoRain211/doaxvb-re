#ifndef DOAXBV_RECOMP_ATTRACT_SEQUENCE_H
#define DOAXBV_RECOMP_ATTRACT_SEQUENCE_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Takes the pre-rendered movies out of the idle attract sequence. */
void recomp_attract_sequence_frame(void);
RecompFunction recomp_attract_sequence_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
