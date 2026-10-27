#ifndef DOAXBV_RECOMP_UI_LOG_H
#define DOAXBV_RECOMP_UI_LOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Whether RECOMP_AUTO_LOG=1 asked for the screen and cursor log. */
bool recomp_ui_log_enabled(void);

/* Logs which screen the game shows and where its cursor is. Read-only. */
void recomp_ui_log_frame(void);

#ifdef __cplusplus
}
#endif

#endif
