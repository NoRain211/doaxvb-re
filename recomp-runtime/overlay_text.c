#include "overlay_text.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

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
static atomic_flag published_lock = ATOMIC_FLAG_INIT;

/* The copy is a few hundred bytes, so a spin lock held only for it is
   enough between the game thread and a render thread. */
static void lock(void)
{
    while (atomic_flag_test_and_set_explicit(&published_lock, memory_order_acquire)) {
    }
}

static void unlock(void)
{
    atomic_flag_clear_explicit(&published_lock, memory_order_release);
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
