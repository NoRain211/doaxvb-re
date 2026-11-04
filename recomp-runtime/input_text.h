/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOAXBV_RECOMP_INPUT_TEXT_H
#define DOAXBV_RECOMP_INPUT_TEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Typed ASCII characters, including Enter, Backspace and Esc, from the
   window's character events, for the runner's own text fields. Up to 64
   wait; take moves them out. From doa2u-re's input_keyboard_win32.c. */
void recomp_input_text_char(uint32_t character);
size_t recomp_input_text_take(char *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif

