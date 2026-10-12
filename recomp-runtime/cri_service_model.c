#include "cri_service_model.h"
#include <stddef.h>

void recomp_cri_service_reset(RecompCriServiceModel *model)
{
    if (model != NULL) {
        *model = (RecompCriServiceModel){0};
    }
}

RecompCriServiceResult recomp_cri_service_run_lane2(
    RecompCriServiceModel *model,
    RecompCriServiceStep step,
    void *context)
{
    if (model == NULL || step == NULL) {
        return RECOMP_CRI_SERVICE_INVALID;
    }
    if (model->lane2_active != 0u) {
        ++model->lane2_busy_skips;
        return RECOMP_CRI_SERVICE_BUSY;
    }

    model->lane2_active = 1u;
    step(context);
    model->lane2_active = 0u;
    ++model->lane2_batches;
    return RECOMP_CRI_SERVICE_OK;
}

RecompCriServiceResult recomp_cri_service_run_lane5(
    RecompCriServiceModel *model,
    RecompCriServiceStep step,
    void *context)
{
    if (model == NULL || step == NULL) {
        return RECOMP_CRI_SERVICE_INVALID;
    }
    if (model->lane5_active != 0u) {
        ++model->lane5_busy_skips;
        return RECOMP_CRI_SERVICE_BUSY;
    }

    model->lane5_active = 1u;
    step(context);
    model->lane5_active = 0u;
    ++model->lane5_handoffs;
    return RECOMP_CRI_SERVICE_OK;
}
