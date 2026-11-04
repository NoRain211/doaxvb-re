/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Adapted from doa2u-re's overlay.c: the queue and the font are the same;
   the presenter's thread draws the published quads (overlay_imgui_panel.cpp). */
#include "overlay_screen.h"

#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <stdatomic.h>
#endif

static RecompOverlayQuad quads[RECOMP_OVERLAY_MAX_QUADS];
static uint32_t quad_count;

/* The built-in font for ' ' through '_': seven rows of five dots each, top
   to bottom, drawn for this runner. */
static const char font[64][42] = {
    ".....|.....|.....|.....|.....|.....|.....", /* space */
    "..#..|..#..|..#..|..#..|..#..|.....|..#..", /* ! */
    ".#.#.|.#.#.|.....|.....|.....|.....|.....", /* " */
    ".#.#.|#####|.#.#.|.#.#.|#####|.#.#.|.....", /* # */
    "..#..|.####|#.#..|.###.|..#.#|####.|..#..", /* $ */
    "##...|##..#|...#.|..#..|.#...|#..##|...##", /* % */
    ".##..|#..#.|#.#..|.#...|#.#.#|#..#.|.##.#", /* & */
    "..#..|..#..|.....|.....|.....|.....|.....", /* ' */
    "...#.|..#..|.#...|.#...|.#...|..#..|...#.", /* ( */
    ".#...|..#..|...#.|...#.|...#.|..#..|.#...", /* ) */
    ".....|..#..|#.#.#|.###.|#.#.#|..#..|.....", /* * */
    ".....|..#..|..#..|#####|..#..|..#..|.....", /* + */
    ".....|.....|.....|.....|.##..|..#..|.#...", /* , */
    ".....|.....|.....|#####|.....|.....|.....", /* - */
    ".....|.....|.....|.....|.....|.##..|.##..", /* . */
    "....#|....#|...#.|..#..|.#...|#....|#....", /* / */
    ".###.|#...#|#..##|#.#.#|##..#|#...#|.###.", /* 0 */
    "..#..|.##..|..#..|..#..|..#..|..#..|.###.", /* 1 */
    ".###.|#...#|....#|...#.|..#..|.#...|#####", /* 2 */
    "#####|...#.|..#..|...#.|....#|#...#|.###.", /* 3 */
    "...#.|..##.|.#.#.|#..#.|#####|...#.|...#.", /* 4 */
    "#####|#....|####.|....#|....#|#...#|.###.", /* 5 */
    "..##.|.#...|#....|####.|#...#|#...#|.###.", /* 6 */
    "#####|....#|...#.|..#..|.#...|.#...|.#...", /* 7 */
    ".###.|#...#|#...#|.###.|#...#|#...#|.###.", /* 8 */
    ".###.|#...#|#...#|.####|....#|...#.|.##..", /* 9 */
    ".....|.##..|.##..|.....|.##..|.##..|.....", /* : */
    ".....|.##..|.##..|.....|.##..|..#..|.#...", /* ; */
    "...#.|..#..|.#...|#....|.#...|..#..|...#.", /* < */
    ".....|.....|#####|.....|#####|.....|.....", /* = */
    ".#...|..#..|...#.|....#|...#.|..#..|.#...", /* > */
    ".###.|#...#|....#|...#.|..#..|.....|..#..", /* ? */
    ".###.|#...#|....#|.##.#|#.#.#|#.#.#|.###.", /* @ */
    ".###.|#...#|#...#|#...#|#####|#...#|#...#", /* A */
    "####.|#...#|#...#|####.|#...#|#...#|####.", /* B */
    ".###.|#...#|#....|#....|#....|#...#|.###.", /* C */
    "###..|#..#.|#...#|#...#|#...#|#..#.|###..", /* D */
    "#####|#....|#....|####.|#....|#....|#####", /* E */
    "#####|#....|#....|####.|#....|#....|#....", /* F */
    ".###.|#...#|#....|#.###|#...#|#...#|.####", /* G */
    "#...#|#...#|#...#|#####|#...#|#...#|#...#", /* H */
    ".###.|..#..|..#..|..#..|..#..|..#..|.###.", /* I */
    "..###|...#.|...#.|...#.|...#.|#..#.|.##..", /* J */
    "#...#|#..#.|#.#..|##...|#.#..|#..#.|#...#", /* K */
    "#....|#....|#....|#....|#....|#....|#####", /* L */
    "#...#|##.##|#.#.#|#.#.#|#...#|#...#|#...#", /* M */
    "#...#|#...#|##..#|#.#.#|#..##|#...#|#...#", /* N */
    ".###.|#...#|#...#|#...#|#...#|#...#|.###.", /* O */
    "####.|#...#|#...#|####.|#....|#....|#....", /* P */
    ".###.|#...#|#...#|#...#|#.#.#|#..#.|.##.#", /* Q */
    "####.|#...#|#...#|####.|#.#..|#..#.|#...#", /* R */
    ".####|#....|#....|.###.|....#|....#|####.", /* S */
    "#####|..#..|..#..|..#..|..#..|..#..|..#..", /* T */
    "#...#|#...#|#...#|#...#|#...#|#...#|.###.", /* U */
    "#...#|#...#|#...#|#...#|#...#|.#.#.|..#..", /* V */
    "#...#|#...#|#...#|#.#.#|#.#.#|#.#.#|.#.#.", /* W */
    "#...#|#...#|.#.#.|..#..|.#.#.|#...#|#...#", /* X */
    "#...#|#...#|#...#|.#.#.|..#..|..#..|..#..", /* Y */
    "#####|....#|...#.|..#..|.#...|#....|#####", /* Z */
    ".###.|.#...|.#...|.#...|.#...|.#...|.###.", /* [ */
    "#....|#....|.#...|..#..|...#.|....#|....#", /* backslash */
    ".###.|...#.|...#.|...#.|...#.|...#.|.###.", /* ] */
    "..#..|.#.#.|#...#|.....|.....|.....|.....", /* ^ */
    ".....|.....|.....|.....|.....|.....|#####", /* _ */
};

void recomp_overlay_rect(float x, float y, float width, float height, uint32_t argb)
{
    if (quad_count == RECOMP_OVERLAY_MAX_QUADS || width <= 0.0f || height <= 0.0f) return;
    quads[quad_count++] = (RecompOverlayQuad){x, y, width, height, argb};
}

static const char *glyph(unsigned char c)
{
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
    return font[c >= 32u && c < 96u ? c - 32u : '?' - 32u];
}

float recomp_overlay_text_width(const char *text, float scale)
{
    const size_t length = strlen(text);
    return length == 0u ? 0.0f : (float)(length * 6u - 1u) * scale;
}

float recomp_overlay_text(float x, float y, float scale, uint32_t argb, const char *text)
{
    for (const unsigned char *c = (const unsigned char *)text; *c != 0u; ++c, x += 6.0f * scale) {
        const char *dots = glyph(*c);
        for (unsigned row = 0u; row < 7u; ++row) {
            const char *line = dots + row * 6u;
            /* One quad per run of lit dots. */
            for (unsigned dot = 0u; dot < 5u;) {
                if (line[dot] != '#') {
                    ++dot;
                    continue;
                }
                unsigned end = dot + 1u;
                while (end < 5u && line[end] == '#') ++end;
                recomp_overlay_rect(x + (float)dot * scale, y + (float)row * scale,
                    (float)(end - dot) * scale, scale, argb);
                dot = end;
            }
        }
    }
    return recomp_overlay_text_width(text, scale);
}


static RecompOverlayQuad published[RECOMP_OVERLAY_MAX_QUADS];
static uint32_t published_count;
#ifdef _WIN32
static SRWLOCK published_lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&published_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&published_lock)
#else
static atomic_flag published_lock = ATOMIC_FLAG_INIT;
#define LOCK() while (atomic_flag_test_and_set_explicit(&published_lock, memory_order_acquire)) {}
#define UNLOCK() atomic_flag_clear_explicit(&published_lock, memory_order_release)
#endif

void recomp_overlay_screen_publish(void)
{
    LOCK();
    memcpy(published, quads, quad_count * sizeof quads[0]);
    published_count = quad_count;
    UNLOCK();
    quad_count = 0u;
}

uint32_t recomp_overlay_screen_take(RecompOverlayQuad *out, uint32_t capacity)
{
    LOCK();
    const uint32_t count = published_count < capacity ? published_count : capacity;
    memcpy(out, published, count * sizeof published[0]);
    UNLOCK();
    return count;
}

