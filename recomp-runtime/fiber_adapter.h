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
/* Saved registers of suspended fibres, for whole-game display passes. A
   rollback requires the same set of fibres as at the checkpoint. */
void recomp_fiber_adapter_checkpoint(void);
void recomp_fiber_adapter_rollback(void);
/* Guest fiber contexts, the current fiber and each parked fiber's native
   continuation, for rollback snapshots. Restore only while the fiber that
   saved runs. */
size_t recomp_fiber_adapter_state_size(void);
void recomp_fiber_adapter_state_save(void *state);
void recomp_fiber_adapter_state_restore(const void *state);
/* False when a saved fiber's continuation is gone. */
bool recomp_fiber_adapter_state_restorable(const void *state);
/* Compares the guest-visible fiber model and slot bookkeeping. */
bool recomp_fiber_adapter_state_equal(const void *left, const void *right, size_t size);
/* Guest stacks owned by active XAPI fibers; the stack is scratch, not state. */
typedef struct RecompFiberStackRange {
    uint32_t base;
    uint32_t size;
} RecompFiberStackRange;
size_t recomp_fiber_adapter_stack_ranges(RecompFiberStackRange *ranges, size_t capacity);

/* Consulted on every SwitchToFiber with the running fiber and the target.
   Returning true declines the switch: the call returns at once, as if the
   target had run and switched straight back. */
typedef bool (*RecompFiberSwitchHook)(uint32_t outgoing_handle, uint32_t target_handle);
void recomp_fiber_set_switch_hook(RecompFiberSwitchHook hook);

#ifdef __cplusplus
}
#endif

#endif
