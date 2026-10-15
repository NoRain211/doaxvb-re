#ifndef DOAXBV_RECOMP_VIEWER_MENU_ADAPTER_H
#define DOAXBV_RECOMP_VIEWER_MENU_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The main menu's MODEL VIEWER item; see viewer_menu_adapter.c. */
RecompFunction recomp_viewer_menu_lookup_manual(uint32_t guest_address);
/* True from choosing MODEL VIEWER until the viewer is done with it. */
bool recomp_viewer_menu_requested(void);
void recomp_viewer_menu_clear(void);
/* Hooks the viewer's once-a-frame work in; the runner calls it at start. */
void recomp_viewer_install(void);

#ifdef __cplusplus
}
#endif

#endif
