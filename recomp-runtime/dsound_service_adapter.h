#ifndef DOAXBV_RECOMP_DSOUND_SERVICE_ADAPTER_H
#define DOAXBV_RECOMP_DSOUND_SERVICE_ADAPTER_H

#include "dsound_service_model.h"
#include "runtime.h"

RecompFunction recomp_dsound_service_lookup_manual(uint32_t guest_address);

/* Test seam: reset and inspect the file-static model. */
void recomp_dsound_service_adapter_reset(void);
const RecompDsoundServiceModel *recomp_dsound_service_adapter_model(void);
/* Rollback snapshots; they take the lock the output pump shares. */
size_t recomp_dsound_service_adapter_state_size(void);
void recomp_dsound_service_adapter_state_save(void *state);
void recomp_dsound_service_adapter_state_restore(const void *state);
/* Host output only: guest buffers keep their clocks while muted. */
void recomp_dsound_service_adapter_set_muted(int muted);

#endif
