#ifndef DOAXBV_RECOMP_CRI_SERVICE_ADAPTER_H
#define DOAXBV_RECOMP_CRI_SERVICE_ADAPTER_H

#include "cri_service_model.h"
#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompCriServiceHooks {
    RecompFunction lane2_service;
    RecompFunction file_worker_io;
    RecompFunction file_worker_service;
    RecompFunction lane5_service;
    RecompFunction adxf_get_stat;
    RecompFunction adxf_get_pt_stat;
    RecompFunction adxf_open;
    RecompFunction adxt_get_stat;
} RecompCriServiceHooks;

RecompFunction recomp_cri_service_lookup_manual(uint32_t guest_address);
uint32_t recomp_cri_service_adxf_get_stat_calls(void);
uint32_t recomp_cri_service_file_worker_steps(void);
void recomp_cri_service_adapter_reset(void);
void recomp_cri_service_adapter_set_hooks(
    const RecompCriServiceHooks *hooks);
const RecompCriServiceModel *recomp_cri_service_adapter_model(void);

#ifdef __cplusplus
}
#endif

#endif
