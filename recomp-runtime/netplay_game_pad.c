/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_game.h"

#include <stddef.h>

/* USA executable. Only the pad update (0x0B5870) reads the raw XInput state,
   and every other reader takes its processed copy (0x5F3088 + port * 0x2C)
   or the per-player copy of that (0x4D9540 + player * 0x2C):
   - sticks: the pad update zeroes -0x2000 < v < 0x2000 in the copy;
   - A, B, X, Y, Black and White: it zeroes raw bytes under 0x20 (table at
     0x2C02E8) before copying them;
   - L trigger: only its digital bit, set at 0x80 (table at 0x2C0320), is read;
   - R trigger: its digital bit, and its value in 0x020BD0 and 0x020260 (from
     0x020740), which read under 0x32 as zero and scale it from there. */
RecompInputGamepad recomp_netplay_game_canonical_pad(RecompInputGamepad pad)
{
    enum { STICK_ZERO = 0x2000, FACE_ZERO = 0x20u, L_TRIGGER_ZERO = 0x80u, R_TRIGGER_ZERO = 0x32u };
    int16_t *thumbs[] = {&pad.thumb_lx, &pad.thumb_ly, &pad.thumb_rx, &pad.thumb_ry};
    for (size_t i = 0u; i < sizeof thumbs / sizeof *thumbs; ++i)
        if (*thumbs[i] > -STICK_ZERO && *thumbs[i] < STICK_ZERO) *thumbs[i] = 0;
    for (size_t i = 0u; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; ++i) {
        const unsigned zero = i < 6u ? FACE_ZERO : i == 6u ? L_TRIGGER_ZERO : R_TRIGGER_ZERO;
        if (pad.analog_buttons[i] < zero) pad.analog_buttons[i] = 0u;
    }
    return pad;
}
