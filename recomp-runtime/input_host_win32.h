#ifndef DOAXBV_RECOMP_INPUT_HOST_WIN32_H
#define DOAXBV_RECOMP_INPUT_HOST_WIN32_H

#include "input_model.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WIN32
/* Call on the SDL main thread after pumping events, and after window teardown.
   The guest thread consumes an atomic keyboard/focus snapshot. */
void recomp_input_host_capture_keyboard(void);
#endif

bool recomp_input_host_sample(uint32_t port, RecompInputGamepad *gamepad);

void recomp_input_host_set_vibration(
    uint32_t port,
    uint16_t left_motor,
    uint16_t right_motor);
void recomp_input_host_stop_vibration(void);

#ifdef __cplusplus
}
#endif

#endif
