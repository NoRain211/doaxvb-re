#include "save_adapter.h"

#ifdef RECOMP_FULL_PROGRAM
#include "fiber_adapter.h"
#include "kernel_abi.h"
#include "save_transaction.h"
#include "stop_report.h"

#include <inttypes.h>

RecompFunction recomp_lookup(uint32_t guest_address);

static void save_original(uint32_t address, uint32_t success_value)
{
    const uint32_t owner = recomp_fiber_adapter_model()->current_handle;
    const RecompFunction original = recomp_lookup(address);

    if (original == NULL) {
        recomp_stop(1, "save:missing-original:0x%08" PRIx32, address);
        return;
    }
    if (!recomp_save_begin(owner)) {
        recomp_stop(1, "save:begin:0x%08" PRIx32, address);
        return;
    }

    /* Raw dispatch preserves the original ABI and cannot reenter this hook.
       Nested region saves join this fiber's transaction even when the
       enclosing guest routine ignores their return value. */
    original();
    if (!recomp_kernel_save_handles_closed(owner)) {
        recomp_save_note_failure(owner);
    }
    const bool success = recomp_runtime.registers.eax == success_value;
    if (recomp_save_end_recovers(owner, success)) {
        recomp_kernel_release_profile_handles();
    }
    if (!recomp_save_end(owner, success)) {
        recomp_stop(1, "save:end:0x%08" PRIx32, address);
    }
}

static void save_profile(void)
{
    save_original(0x000e0410u, 1u);
}

static void save_profile_region(void)
{
    save_original(0x0001adb0u, 0u);
}
#endif

RecompFunction recomp_save_lookup_manual(uint32_t guest_address)
{
#ifdef RECOMP_FULL_PROGRAM
    switch (guest_address) {
    case 0x000e0410u:
        return save_profile;
    case 0x0001adb0u:
        return save_profile_region;
    default:
        return NULL;
    }
#else
    (void)guest_address;
    return NULL;
#endif
}
