#ifndef DOAXBV_RECOMP_INPUT_MODEL_H
#define DOAXBV_RECOMP_INPUT_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RECOMP_INPUT_PORT_COUNT = 4u,
    RECOMP_INPUT_ANALOG_BUTTON_COUNT = 8u,
};

typedef struct RecompInputGamepad {
    uint16_t buttons;
    uint8_t analog_buttons[RECOMP_INPUT_ANALOG_BUTTON_COUNT];
    int16_t thumb_lx;
    int16_t thumb_ly;
    int16_t thumb_rx;
    int16_t thumb_ry;
} RecompInputGamepad;

typedef struct RecompInputPort {
    bool open;
    uint32_t packet_number;
    RecompInputGamepad gamepad;
    uint16_t left_motor;
    uint16_t right_motor;
} RecompInputPort;

typedef struct RecompInputModel {
    uint32_t connected_mask;
    uint32_t reported_mask;
    RecompInputPort ports[RECOMP_INPUT_PORT_COUNT];
} RecompInputModel;

void recomp_input_reset(RecompInputModel *model, uint32_t connected_mask);
void recomp_input_set_connected(
    RecompInputModel *model,
    uint32_t port,
    bool connected);
uint32_t recomp_input_get_devices(RecompInputModel *model);
bool recomp_input_get_device_changes(
    RecompInputModel *model,
    uint32_t *insertions,
    uint32_t *removals);
uint32_t recomp_input_open(RecompInputModel *model, uint32_t port);
void recomp_input_close(RecompInputModel *model, uint32_t handle);
bool recomp_input_handle_port(uint32_t handle, uint32_t *port);
bool recomp_input_set_gamepad(
    RecompInputModel *model,
    uint32_t handle,
    const RecompInputGamepad *gamepad);
bool recomp_input_get_state(
    const RecompInputModel *model,
    uint32_t handle,
    uint32_t *packet_number,
    RecompInputGamepad *gamepad);
bool recomp_input_set_feedback(
    RecompInputModel *model,
    uint32_t handle,
    uint16_t left_motor,
    uint16_t right_motor);

#ifdef __cplusplus
}
#endif

#endif
