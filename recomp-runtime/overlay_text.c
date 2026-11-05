#include "overlay_text.h"
#include "overlay_screen.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <stdatomic.h>
#endif

static void copy_text(char *destination, size_t size, const char *source)
{
    if (source == NULL) source = "";
    size_t length = strlen(source);
    if (length >= size) length = size - 1u;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

void recomp_overlay_clear(RecompOverlayText *text)
{
    if (text != NULL) memset(text, 0, sizeof *text);
}

void recomp_overlay_set_title(RecompOverlayText *text, const char *title)
{
    if (text != NULL) copy_text(text->title, sizeof text->title, title);
}

bool recomp_overlay_add_line(RecompOverlayText *text, const char *format, ...)
{
    va_list arguments;

    if (text == NULL || format == NULL || text->count >= RECOMP_OVERLAY_LINES) return false;
    va_start(arguments, format);
    vsnprintf(text->line[text->count], sizeof text->line[text->count], format, arguments);
    va_end(arguments);
    ++text->count;
    return true;
}

void recomp_overlay_set_hint(RecompOverlayText *text, const char *hint)
{
    if (text != NULL) copy_text(text->hint, sizeof text->hint, hint);
}

bool recomp_overlay_empty(const RecompOverlayText *text)
{
    return text == NULL || (text->title[0] == '\0' && text->count == 0u && text->hint[0] == '\0');
}

static RecompOverlayText published;
/* The game's queue of shapes (overlay_screen.h) and the screen it last published. */
static RecompOverlayQuad quads[RECOMP_OVERLAY_MAX_QUADS], published_quads[RECOMP_OVERLAY_MAX_QUADS];
static uint32_t quad_count, published_quad_count;
#ifdef _WIN32
static volatile LONG published_lock;
#else
static atomic_flag published_lock = ATOMIC_FLAG_INIT;
#endif

/* Each copy is the text or at most one screen of quads, so a spin lock held
   only for it is enough between the game thread and a render thread. */
static void lock(void)
{
#ifdef _WIN32
    while (InterlockedExchange(&published_lock, 1) != 0) {
    }
#else
    while (atomic_flag_test_and_set_explicit(&published_lock, memory_order_acquire)) {
    }
#endif
}

static void unlock(void)
{
#ifdef _WIN32
    InterlockedExchange(&published_lock, 0);
#else
    atomic_flag_clear_explicit(&published_lock, memory_order_release);
#endif
}

void recomp_overlay_publish(const RecompOverlayText *text)
{
    if (text == NULL) return;
    lock();
    published = *text;
    unlock();
}

void recomp_overlay_take(RecompOverlayText *text)
{
    if (text == NULL) return;
    lock();
    *text = published;
    unlock();
}

void recomp_overlay_rect(float x, float y, float width, float height, uint32_t argb)
{
    if (quad_count == RECOMP_OVERLAY_MAX_QUADS || width <= 0.0f || height <= 0.0f) return;
    quads[quad_count++] = (RecompOverlayQuad){x, y, width, height, argb};
}

void recomp_overlay_screen_publish(void)
{
    lock();
    memcpy(published_quads, quads, quad_count * sizeof quads[0]);
    published_quad_count = quad_count;
    unlock();
    quad_count = 0u;
}

uint32_t recomp_overlay_screen_take(RecompOverlayQuad *out, uint32_t capacity)
{
    lock();
    const uint32_t count = published_quad_count < capacity ? published_quad_count : capacity;
    memcpy(out, published_quads, count * sizeof out[0]);
    unlock();
    return count;
}
