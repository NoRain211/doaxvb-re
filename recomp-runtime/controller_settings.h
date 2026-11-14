#ifndef RECOMP_CONTROLLER_SETTINGS_H
#define RECOMP_CONTROLLER_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
bool recomp_controller_settings_load(const char *disc_root, uint8_t modes[4]);
bool recomp_controller_settings_save(const char *disc_root, const uint8_t modes[4]);
/* Voice then Message language, each 0 Japanese or 1 English. Load returns 1
   when read, 0 when none has been chosen and -1 when the file is invalid. */
int recomp_language_settings_load(const char *disc_root, uint8_t languages[2]);
bool recomp_language_settings_save(const char *disc_root, const uint8_t languages[2]);
#ifdef __cplusplus
}
#endif
#endif
