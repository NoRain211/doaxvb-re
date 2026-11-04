#ifndef DOAXBV_RECOMP_INPUT_ADAPTER_H
#define DOAXBV_RECOMP_INPUT_ADAPTER_H

#include "input_model.h"
#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns false when no pad is present on the port. */
typedef bool (*RecompInputSampleSource)(
    uint32_t port,
    RecompInputGamepad *gamepad);

RecompFunction recomp_input_lookup_manual(uint32_t guest_address);
void recomp_input_adapter_reset(void);
void recomp_input_adapter_set_source(RecompInputSampleSource source);

typedef void (*RecompInputFeedbackSink)(
    uint32_t port,
    uint16_t left_motor,
    uint16_t right_motor);
void recomp_input_adapter_set_feedback_sink(RecompInputFeedbackSink sink);
const RecompInputModel *recomp_input_adapter_model(void);
/* Rollback snapshots of the open handles, packet numbers and last pads. */
size_t recomp_input_adapter_state_size(void);
void recomp_input_adapter_state_save(void *state);
void recomp_input_adapter_state_restore(const void *state);

#ifdef __cplusplus
}
#endif

#endif
