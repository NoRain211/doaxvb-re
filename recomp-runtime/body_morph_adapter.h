#ifndef DOAXBV_RECOMP_BODY_MORPH_ADAPTER_H
#define DOAXBV_RECOMP_BODY_MORPH_ADAPTER_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The game's blend of one morph record (a girl's hand gesture, face or
   chest), readable (docs/body-morph.md, body_morph.h). Registered for
   0x00064FD0, which the recipe routes through the manual lookup.
   RECOMP_BODY_MORPH_VERIFY=1 checks it against the generated code each
   call. */
RecompFunction recomp_body_morph_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
