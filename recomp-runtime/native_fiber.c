#include "native_fiber.h"
#include "runtime.h"
#include "stop_report.h"

#ifdef _WIN32

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    NATIVE_STACK_BYTES = 0x100000u,
    NATIVE_STACK_GUARD = 0x1000u,
};

/* Windows fibers hide their suspended context in OS storage, which a
   snapshot cannot reach. This keeps the context and the live stack itself:
   a switch captures the caller's context and restores the target's, so the
   whole continuation is plain data. */
typedef struct RecompNativeFiber {
    CONTEXT context;
    PVOID exception_list;
    uint8_t *stack_memory;
    size_t stack_allocation;
    uint8_t *stack_base;
    uint8_t *stack_limit;
    LPFIBER_START_ROUTINE entry;
    void *parameter;
    bool started;
    bool suspended;
} RecompNativeFiber;

#if defined(_MSC_VER)
#define RECOMP_THREAD_LOCAL __declspec(thread)
#else
#define RECOMP_THREAD_LOCAL _Thread_local
#endif

/* Each host thread runs its own guest threads, so the current fiber is
   per-thread just as the old GetCurrentFiber() was. */
static RECOMP_THREAD_LOCAL RecompNativeFiber *active_fiber;

static NT_TIB *current_tib(void)
{
    return (NT_TIB *)NtCurrentTeb();
}

static void set_stack_limits(const RecompNativeFiber *fiber)
{
    NT_TIB *tib = current_tib();
    tib->StackBase = fiber->stack_base;
    tib->StackLimit = fiber->stack_limit;
}

static void fiber_trampoline(void *parameter)
{
    RecompNativeFiber *fiber = parameter;
    fiber->entry(fiber->parameter);
    recomp_stop(2, "fiber:trampoline-returned");
}

static RecompNativeFiber *current_fiber(void)
{
    if (active_fiber == NULL) {
        RecompNativeFiber *fiber = calloc(1u, sizeof *fiber);
        NT_TIB *tib = current_tib();
        if (fiber == NULL) recomp_stop(2, "fiber:convert-failed");
        fiber->stack_base = (uint8_t *)tib->StackBase;
        fiber->stack_limit = (uint8_t *)tib->StackLimit;
        fiber->started = true;
        active_fiber = fiber;
    }
    return active_fiber;
}

static void start_fiber(RecompNativeFiber *fiber)
{
    CONTEXT context;
    memset(&context, 0, sizeof context);
    RtlCaptureContext(&context);
    context.ContextFlags = CONTEXT_FULL;
    context.Rip = (DWORD64)(uintptr_t)fiber_trampoline;
    context.Rsp = (DWORD64)(uintptr_t)(fiber->stack_base - 0x28u);
    context.Rcx = (DWORD64)(uintptr_t)fiber;
    *(uint64_t *)(uintptr_t)context.Rsp = 0u;
    fiber->started = true;
    set_stack_limits(fiber);
    active_fiber = fiber;
    RtlRestoreContext(&context, NULL);
}

static void *native_current(void)
{
    return current_fiber();
}

void *recomp_native_fiber_create(LPFIBER_START_ROUTINE entry, void *parameter)
{
    RecompNativeFiber *fiber = calloc(1u, sizeof *fiber);
    const size_t allocation = NATIVE_STACK_BYTES + NATIVE_STACK_GUARD;
    DWORD previous;
    uint8_t *memory;

    if (fiber == NULL) return NULL;
    memory = VirtualAlloc(NULL, allocation, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (memory == NULL) {
        free(fiber);
        return NULL;
    }
    if (!VirtualProtect(memory, NATIVE_STACK_GUARD, PAGE_NOACCESS, &previous)) {
        VirtualFree(memory, 0u, MEM_RELEASE);
        free(fiber);
        return NULL;
    }
    fiber->stack_memory = memory;
    fiber->stack_allocation = allocation;
    fiber->stack_limit = memory + NATIVE_STACK_GUARD;
    fiber->stack_base = memory + allocation;
    fiber->entry = entry;
    fiber->parameter = parameter;
    return fiber;
}

static void native_switch(void *target)
{
    RecompNativeFiber *from = current_fiber();
    RecompNativeFiber *to = target;

    memset(&from->context, 0, sizeof from->context);
    RtlCaptureContext(&from->context);
    if (from->suspended) {
        /* Resumed by another fiber; this capture is the next snapshot point. */
        from->suspended = false;
        current_tib()->ExceptionList = from->exception_list;
        set_stack_limits(from);
        return;
    }
    from->exception_list = current_tib()->ExceptionList;
    from->suspended = true;
    active_fiber = to;
    set_stack_limits(to);
    current_tib()->ExceptionList = to->exception_list;
    if (to->started) {
        RtlRestoreContext(&to->context, NULL);
    } else {
        start_fiber(to);
    }
    recomp_stop(2, "fiber:restore-returned");
}

void recomp_native_fiber_delete(void *handle)
{
    RecompNativeFiber *fiber = handle;
    if (fiber == NULL) return;
    if (fiber->stack_memory != NULL)
        VirtualFree(fiber->stack_memory, 0u, MEM_RELEASE);
    if (active_fiber == fiber) active_fiber = NULL;
    free(fiber);
}

void recomp_native_fiber_restart(void *handle, LPFIBER_START_ROUTINE entry, void *parameter)
{
    RecompNativeFiber *fiber = handle;
    if (fiber == NULL) return;
    fiber->entry = entry;
    fiber->parameter = parameter;
    fiber->started = false;
    fiber->suspended = false;
    memset(&fiber->context, 0, sizeof fiber->context);
    fiber->exception_list = NULL;
}

void recomp_native_fiber_release_thread(void)
{
    if (active_fiber != NULL && active_fiber->stack_memory == NULL) {
        free(active_fiber);
        active_fiber = NULL;
    }
}

size_t recomp_native_fiber_state_size(void)
{
    return sizeof(RecompNativeFiberState);
}

bool recomp_native_fiber_owns_stack(void *handle)
{
    const RecompNativeFiber *fiber = handle;
    return fiber != NULL && fiber->stack_memory != NULL;
}

void recomp_native_fiber_state_save(void *handle, RecompNativeFiberState *state)
{
    RecompNativeFiber *fiber = handle;
    const uint8_t *rsp;
    size_t used;

    memset(state, 0, sizeof *state);
    if (fiber == NULL || fiber->stack_memory == NULL) return;
    if (fiber->started) state->flags |= RECOMP_NATIVE_STATE_STARTED;
    if (!fiber->started) return;
    rsp = (const uint8_t *)(uintptr_t)fiber->context.Rsp;
    if (rsp < fiber->stack_limit || rsp > fiber->stack_base) {
        state->flags |= RECOMP_NATIVE_STATE_UNAVAILABLE;
        return;
    }
    used = (size_t)(fiber->stack_base - rsp);
    if (used > RECOMP_NATIVE_STACK_SAVE_MAX) {
        state->flags |= RECOMP_NATIVE_STATE_UNAVAILABLE;
        return;
    }
    state->context = fiber->context;
    state->exception_list = fiber->exception_list;
    state->stack_bytes = (uint32_t)used;
    state->flags |= RECOMP_NATIVE_STATE_STACK;
    memcpy(state->stack, rsp, used);
}

bool recomp_native_fiber_state_restore(void *handle, const RecompNativeFiberState *state)
{
    RecompNativeFiber *fiber = handle;
    uint8_t *rsp;

    if (fiber == NULL) return true;
    fiber->suspended = false;
    fiber->started = (state->flags & RECOMP_NATIVE_STATE_STARTED) != 0u;
    if ((state->flags & RECOMP_NATIVE_STATE_STACK) == 0u) return true;
    if (state->stack_bytes > RECOMP_NATIVE_STACK_SAVE_MAX) return false;
    rsp = (uint8_t *)(uintptr_t)state->context.Rsp;
    if (rsp < fiber->stack_limit || rsp > fiber->stack_base) return false;
    if ((size_t)(fiber->stack_base - rsp) < state->stack_bytes) return false;
    memcpy(rsp, state->stack, state->stack_bytes);
    fiber->context = state->context;
    fiber->exception_list = state->exception_list;
    fiber->suspended = true;
    return true;
}

#else

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* POSIX hosts switch stacks with recomp_fiber_switch (fiber_switch_*.S). It
   saves the callee-saved registers on the outgoing stack and resumes the
   target's saved stack pointer. A new fiber's frame returns into
   recomp_fiber_trampoline, which calls entry(parameter). */
void recomp_fiber_switch(void **from_sp, void *to_sp);
void recomp_fiber_trampoline(void);

typedef struct RecompNativeFiber {
    void *sp;
    void *stack_memory;
    size_t stack_size;
} RecompNativeFiber;

static _Thread_local RecompNativeFiber *active_fiber;

static void *native_current(void)
{
    if (active_fiber == NULL) {
        active_fiber = calloc(1u, sizeof(*active_fiber));
    }
    return active_fiber;
}

void *recomp_native_fiber_create(LPFIBER_START_ROUTINE entry, void *parameter)
{
    /* The lowest page stays inaccessible, so an overflow faults here as it
       does against a Windows fiber's guard page. */
    const size_t guard_size = (size_t)sysconf(_SC_PAGESIZE);
    const size_t stack_size = 0x100000u + guard_size;
    void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    RecompNativeFiber *fiber;

    if (stack == MAP_FAILED) return NULL;
    if (mprotect(stack, guard_size, PROT_NONE) != 0) {
        munmap(stack, stack_size);
        return NULL;
    }
    fiber = calloc(1u, sizeof(*fiber));
    if (fiber == NULL) {
        munmap(stack, stack_size);
        return NULL;
    }
    fiber->stack_memory = stack;
    fiber->stack_size = stack_size;
    recomp_native_fiber_restart(fiber, entry, parameter);
    return fiber;
}

void recomp_native_fiber_restart(void *handle, LPFIBER_START_ROUTINE entry, void *parameter)
{
    RecompNativeFiber *fiber = handle;
    uintptr_t top = ((uintptr_t)fiber->stack_memory + fiber->stack_size) & ~(uintptr_t)15u;
    uint64_t *frame;

#if defined(__arm64__) || defined(__aarch64__)
    /* d8-d15 and x19-x30 in the order recomp_fiber_switch pops them. */
    top -= 160u;
    frame = (uint64_t *)top;
    for (size_t i = 0u; i < 20u; ++i) frame[i] = 0u;
    frame[9] = (uint64_t)(uintptr_t)recomp_fiber_trampoline; /* x30 */
    frame[18] = (uint64_t)(uintptr_t)parameter; /* x19 */
    frame[19] = (uint64_t)(uintptr_t)entry; /* x20 */
#elif defined(__x86_64__)
    /* r15, r14, r13, r12, rbx, rbp, return address, alignment pad. */
    top -= 64u;
    frame = (uint64_t *)top;
    for (size_t i = 0u; i < 8u; ++i) frame[i] = 0u;
    frame[2] = (uint64_t)(uintptr_t)entry; /* r13 */
    frame[3] = (uint64_t)(uintptr_t)parameter; /* r12 */
    frame[6] = (uint64_t)(uintptr_t)recomp_fiber_trampoline;
#else
#error "Unsupported architecture for native POSIX fiber switching"
#endif
    fiber->sp = (void *)top;
}

static void native_switch(void *target)
{
    RecompNativeFiber *from = native_current();

    active_fiber = target;
    recomp_fiber_switch(&from->sp, active_fiber->sp);
}

void recomp_native_fiber_delete(void *handle)
{
    RecompNativeFiber *fiber = handle;

    if (fiber == NULL) return;
    if (fiber->stack_memory != NULL) {
        munmap(fiber->stack_memory, fiber->stack_size);
    }
    if (active_fiber == fiber) active_fiber = NULL;
    free(fiber);
}

void recomp_native_fiber_release_thread(void)
{
    free(active_fiber);
    active_fiber = NULL;
}

/* shortcut: POSIX switches keep the continuation on the fiber stack, but it
   is not captured, so every parked fiber reports itself unrewindable and
   SyncTest skips; add stack capture when rollback targets Linux or macOS. */
size_t recomp_native_fiber_state_size(void)
{
    return sizeof(RecompNativeFiberState);
}

bool recomp_native_fiber_owns_stack(void *fiber)
{
    const RecompNativeFiber *native = fiber;
    return native != NULL && native->stack_memory != NULL;
}

void recomp_native_fiber_state_save(void *fiber, RecompNativeFiberState *state)
{
    memset(state, 0, sizeof *state);
    if (recomp_native_fiber_owns_stack(fiber)) state->flags = RECOMP_NATIVE_STATE_UNAVAILABLE;
}

bool recomp_native_fiber_state_restore(void *fiber, const RecompNativeFiberState *state)
{
    (void)fiber;
    (void)state;
    return true;
}

#endif

void *recomp_native_fiber_current(void)
{
    void *fiber = native_current();
    if (fiber == NULL) recomp_stop(2, "fiber:convert-failed");
    return fiber;
}

void recomp_native_fiber_switch(void *target)
{
    native_switch(target);
}
