#ifndef DOAXBV_RECOMP_BODY_JOINTS_ADAPTER_H
#define DOAXBV_RECOMP_BODY_JOINTS_ADAPTER_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The game's per-frame bend of a girl's joint pieces, readable
   (docs/body-joints.md, body_joints.h). Registered for 0x00067540, which the
   recipe routes through the manual lookup. RECOMP_BODY_JOINTS_VERIFY=1
   checks it against the generated code each call. */
RecompFunction recomp_body_joints_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
