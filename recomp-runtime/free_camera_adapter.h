#ifndef DOAXBV_RECOMP_FREE_CAMERA_ADAPTER_H
#define DOAXBV_RECOMP_FREE_CAMERA_ADAPTER_H

#include "input_adapter.h"
#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The free camera's input stage: hand it the source the game would have
   read, and install recomp_free_camera_sample in its place. While the free
   camera flies, the game reads a neutral pad on port 0. */
void recomp_free_camera_set_source(RecompInputSampleSource source);
bool recomp_free_camera_sample(uint32_t port, RecompInputGamepad *gamepad);
RecompFunction recomp_free_camera_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
