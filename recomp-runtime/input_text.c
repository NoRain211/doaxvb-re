/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "input_text.h"

#include <string.h>

/* The window receives events on the presenter's thread; the menu reads on the game's. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK lock = SRWLOCK_INIT;
#else
#include <pthread.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define AcquireSRWLockExclusive pthread_mutex_lock
#define ReleaseSRWLockExclusive pthread_mutex_unlock
#endif
static char text[64];
static size_t text_size;

void recomp_input_text_char(uint32_t character)
{
    if (character >= 0x80u) return;
    AcquireSRWLockExclusive(&lock);
    if (text_size < sizeof text) text[text_size++] = (char)character;
    ReleaseSRWLockExclusive(&lock);
}

size_t recomp_input_text_take(char *out, size_t capacity)
{
    AcquireSRWLockExclusive(&lock);
    const size_t size = text_size < capacity ? text_size : capacity;
    memcpy(out, text, size);
    text_size = 0u;
    ReleaseSRWLockExclusive(&lock);
    return size;
}

