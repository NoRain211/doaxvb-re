#ifndef DOAXBV_RECOMP_FIBER_ADAPTER_H
#define DOAXBV_RECOMP_FIBER_ADAPTER_H

#include "fiber_model.h"
#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

RecompFunction recomp_fiber_lookup_manual(uint32_t guest_address);
void recomp_fiber_adapter_reset(void);
const RecompFiberModel *recomp_fiber_adapter_model(void);
void recomp_fiber_adapter_report(void);

/* Consulted on every SwitchToFiber with the running fiber and the target.
   Returning true declines the switch: the call returns at once, as if the
   target had run and switched straight back. */
typedef bool (*RecompFiberSwitchHook)(uint32_t outgoing_handle, uint32_t target_handle);
void recomp_fiber_set_switch_hook(RecompFiberSwitchHook hook);

#ifdef __cplusplus
}
#endif

#endif
