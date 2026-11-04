#ifndef RECOMP_NATIVE_FIBER_H
#define RECOMP_NATIVE_FIBER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*RecompNativeFiberEntry)(void *parameter);

void *recomp_native_fiber_current(void);
void *recomp_native_fiber_create(RecompNativeFiberEntry entry, void *parameter);
/* Reuses a parked fiber's stack for a new entry, so snapshots that still
   name it stay valid. */
void recomp_native_fiber_restart(void *fiber, RecompNativeFiberEntry entry, void *parameter);
void recomp_native_fiber_switch(void *target);
void recomp_native_fiber_delete(void *fiber);
/* Returns the calling thread from fiber mode; the current fiber is freed. */
void recomp_native_fiber_release_thread(void);

/* A fiber's host continuation: the CPU context captured just before it
   switched away and the live part of its native stack. Plain data, so a
   snapshot can rewind it. */
enum { RECOMP_NATIVE_STACK_SAVE_MAX = 0x10000u };
/* Bit 0: host stack captured. Bit 1: entry has started.
   Bit 2: the stack did not fit, so the continuation is not in this state. */
enum {
    RECOMP_NATIVE_STATE_STACK = 1u,
    RECOMP_NATIVE_STATE_STARTED = 2u,
    RECOMP_NATIVE_STATE_UNAVAILABLE = 4u,
};
typedef struct RecompNativeFiberState {
#ifdef _WIN32
    CONTEXT context;
    PVOID exception_list;
#endif
    uint32_t stack_bytes;
    uint32_t flags;
    uint8_t stack[RECOMP_NATIVE_STACK_SAVE_MAX];
} RecompNativeFiberState;
/* False for a converted thread: its stack holds the runner, not the fiber. */
bool recomp_native_fiber_owns_stack(void *fiber);
/* Writes only the header and the used stack bytes; the rest stays stale. */
void recomp_native_fiber_state_save(void *fiber, RecompNativeFiberState *state);
bool recomp_native_fiber_state_restore(void *fiber, const RecompNativeFiberState *state);

#endif
