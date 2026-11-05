/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "input_text.h"

#include <string.h>

/* The window receives events on the presenter's thread; the menu reads on the game's. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&lock)
#define UNLOCK() ReleaseSRWLockExclusive(&lock)
#else
#include <pthread.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)
#endif
static char text[64];
static size_t text_size;
static bool asked;

void recomp_input_text_char(uint32_t character)
{
    if (character >= 0x80u) return;
    LOCK();
    if (text_size < sizeof text) text[text_size++] = (char)character;
    UNLOCK();
}

size_t recomp_input_text_take(char *out, size_t capacity)
{
    LOCK();
    const size_t size = text_size < capacity ? text_size : capacity;
    memcpy(out, text, size);
    text_size = 0u;
    asked = true;
    UNLOCK();
    return size;
}

bool recomp_input_text_asked(void)
{
    LOCK();
    const bool was = asked;
    asked = false;
    UNLOCK();
    return was;
}

