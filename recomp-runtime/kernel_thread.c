#include "kernel_abi.h"

#include <stdio.h>
#include <time.h>

enum {
    DISPATCHER_SIGNAL_STATE = 0x04u,
    TIMER_DUE_TIME = 0x10u,
    TIMER_DPC = 0x20u,
    MAX_EVENTS = 64u,
};

static const uint32_t STATUS_SUCCESS = 0x00000000u;
static const uint32_t STATUS_INVALID_HANDLE = 0xc0000008u;
static const uint32_t STATUS_INVALID_PARAMETER = 0xc000000du;
static const uint32_t SYNTHETIC_THREAD_OBJECT = 0x00740300u;

typedef struct SyntheticEvent {
    uint32_t handle;
    uint32_t type;
    uint32_t signaled;
} SyntheticEvent;

static SyntheticEvent events[MAX_EVENTS];
static uint32_t next_event_handle = 0xbee20000u;
static int32_t current_base_priority;
static uint32_t suspend_count;

static SyntheticEvent *find_event(uint32_t handle)
{
    for (unsigned i = 0u; i < MAX_EVENTS; ++i) {
        if (events[i].handle == handle) {
            return &events[i];
        }
    }
    return NULL;
}

static SyntheticEvent *create_event(uint32_t type, uint32_t initial_state)
{
    for (unsigned i = 0u; i < MAX_EVENTS; ++i) {
        if (events[i].handle == 0u) {
            events[i].handle = next_event_handle;
            events[i].type = type;
            events[i].signaled = initial_state != 0u;
            next_event_handle += 4u;
            return &events[i];
        }
    }
    return NULL;
}

static void report_immediate_wait_once(const char *name, int *reported)
{
    if (!*reported) {
        fprintf(
            stderr,
            "recomp kernel: %s would block; returning immediately in "
            "single-threaded mode\n",
            name);
        *reported = 1;
    }
}

static void bridge_ke_cancel_timer(void)
{
    uint32_t timer = kernel_arg(1u);

    if (timer != 0u) {
        uint32_t dpc = *recomp_memory_u32(timer + TIMER_DPC);

        if (dpc != 0u) {
            (void)recomp_kernel_remove_dpc(dpc);
        }
        *recomp_memory_u32(timer + TIMER_DPC) = 0u;
    }
    kernel_return(1u, 0u);
}

static void bridge_ke_delay_execution_thread(void)
{
    static int reported;

    report_immediate_wait_once("KeDelayExecutionThread", &reported);
    kernel_return(3u, STATUS_SUCCESS);
}

static void bridge_ke_initialize_timer_ex(void)
{
    uint32_t timer = kernel_arg(1u);
    uint32_t timer_type = kernel_arg(2u);

    if (timer != 0u) {
        uint32_t object_type = timer_type == 0u ? 0x08u : 0x09u;

        *recomp_memory_u32(timer) = object_type;
        *recomp_memory_u32(timer + DISPATCHER_SIGNAL_STATE) = 0u;
        *recomp_memory_u32(timer + 0x08u) = timer + 0x08u;
        *recomp_memory_u32(timer + 0x0cu) = timer + 0x08u;
        *recomp_memory_u64(timer + TIMER_DUE_TIME) = 0u;
        *recomp_memory_u32(timer + 0x18u) = 0u;
        *recomp_memory_u32(timer + 0x1cu) = 0u;
        *recomp_memory_u32(timer + TIMER_DPC) = 0u;
        *recomp_memory_u32(timer + 0x24u) = 0u;
    }
    kernel_return(2u, 0u);
}

static void bridge_ke_query_system_time(void)
{
    uint32_t current_time = kernel_arg(1u);

    if (current_time != 0u) {
        struct timespec now;
        uint64_t file_time = 0u;

        if (timespec_get(&now, TIME_UTC) == TIME_UTC) {
            file_time = ((uint64_t)now.tv_sec + 11644473600ull) *
                10000000ull + (uint64_t)now.tv_nsec / 100ull;
        }
        *recomp_memory_u64(current_time) = file_time;
    }
    kernel_return(1u, 0u);
}

static void bridge_ke_query_base_priority_thread(void)
{
    kernel_return(1u, (uint32_t)current_base_priority);
}

static void bridge_ke_set_base_priority_thread(void)
{
    int32_t previous = current_base_priority;

    current_base_priority = (int32_t)kernel_arg(2u);
    kernel_return(2u, (uint32_t)previous);
}

static void bridge_ke_set_disable_boost_thread(void)
{
    kernel_return(2u, 0u);
}

static void bridge_ke_set_event(void)
{
    uint32_t event = kernel_arg(1u);
    uint32_t previous = 0u;

    if (event != 0u) {
        previous = *recomp_memory_u32(event + DISPATCHER_SIGNAL_STATE);
        *recomp_memory_u32(event + DISPATCHER_SIGNAL_STATE) = 1u;
    }
    kernel_return(3u, previous);
}

static void bridge_ke_set_timer(void)
{
    uint32_t timer = kernel_arg(1u);
    uint32_t dpc = kernel_arg(4u);

    if (timer != 0u) {
        *recomp_memory_u32(timer + DISPATCHER_SIGNAL_STATE) = 0u;
        *recomp_memory_u32(timer + TIMER_DUE_TIME) = kernel_arg(2u);
        *recomp_memory_u32(timer + TIMER_DUE_TIME + 4u) = kernel_arg(3u);
        *recomp_memory_u32(timer + TIMER_DPC) = dpc;
    }
    if (dpc != 0u) {
        (void)recomp_kernel_queue_dpc(dpc, 0u, 0u);
    }
    kernel_return(4u, 0u);
}

static void bridge_ke_stall_execution_processor(void)
{
    kernel_return(1u, 0u);
}

static void bridge_ke_wait_for_single_object(void)
{
    static int reported;

    report_immediate_wait_once("KeWaitForSingleObject", &reported);
    kernel_return(5u, STATUS_SUCCESS);
}

static void bridge_nt_create_event(void)
{
    uint32_t handle_pointer = kernel_arg(1u);
    SyntheticEvent *event;

    if (handle_pointer == 0u) {
        kernel_return(4u, STATUS_INVALID_PARAMETER);
        return;
    }
    event = create_event(kernel_arg(3u), kernel_arg(4u));
    if (event == NULL) {
        kernel_return(4u, STATUS_INVALID_PARAMETER);
        return;
    }
    *recomp_memory_u32(handle_pointer) = event->handle;
    kernel_return(4u, STATUS_SUCCESS);
}

static void bridge_nt_resume_thread(void)
{
    uint32_t handle = kernel_arg(1u);
    uint32_t previous_pointer = kernel_arg(2u);
    uint32_t previous = suspend_count;

    if (handle == 0u) {
        kernel_return(2u, STATUS_INVALID_HANDLE);
        return;
    }
    if (suspend_count > 0u) {
        --suspend_count;
    }
    if (previous_pointer != 0u) {
        *recomp_memory_u32(previous_pointer) = previous;
    }
    kernel_return(2u, STATUS_SUCCESS);
}

static void bridge_nt_set_event(void)
{
    uint32_t handle = kernel_arg(1u);
    uint32_t previous_pointer = kernel_arg(2u);
    SyntheticEvent *event = find_event(handle);
    uint32_t previous = event == NULL ? 0u : event->signaled;

    if (event != NULL) {
        event->signaled = 1u;
    }
    if (previous_pointer != 0u) {
        *recomp_memory_u32(previous_pointer) = previous;
    }
    kernel_return(2u, handle == 0u ? STATUS_INVALID_HANDLE : STATUS_SUCCESS);
}

static void bridge_nt_suspend_thread(void)
{
    static int reported;
    uint32_t handle = kernel_arg(1u);
    uint32_t previous_pointer = kernel_arg(2u);
    uint32_t previous = suspend_count;

    if (handle == 0u) {
        kernel_return(2u, STATUS_INVALID_HANDLE);
        return;
    }
    ++suspend_count;
    if (previous_pointer != 0u) {
        *recomp_memory_u32(previous_pointer) = previous;
    }
    report_immediate_wait_once("NtSuspendThread", &reported);
    kernel_return(2u, STATUS_SUCCESS);
}

static void bridge_nt_wait_for_single_object(void)
{
    static int reported;

    report_immediate_wait_once("NtWaitForSingleObject", &reported);
    kernel_return(3u, STATUS_SUCCESS);
}

static void bridge_nt_wait_for_single_object_ex(void)
{
    static int reported;
    uint32_t handle = kernel_arg(1u);

    report_immediate_wait_once("NtWaitForSingleObjectEx", &reported);
    kernel_return(4u, handle == 0u ? STATUS_INVALID_HANDLE : STATUS_SUCCESS);
}

static void bridge_nt_yield_execution(void)
{
    kernel_return(0u, STATUS_SUCCESS);
}

static void bridge_ob_reference_object_by_handle(void)
{
    uint32_t handle = kernel_arg(1u);
    uint32_t object_pointer = kernel_arg(3u);

    if (object_pointer != 0u) {
        *recomp_memory_u32(object_pointer) =
            handle == 0u ? 0u : SYNTHETIC_THREAD_OBJECT;
    }
    kernel_return(3u, handle == 0u ? STATUS_INVALID_HANDLE : STATUS_SUCCESS);
}

/* ObfDereferenceObject is fastcall: the object arrives in ECX. */
static void bridge_obf_dereference_object(void)
{
    kernel_return_caller_cleanup(0u);
}

RecompFunction recomp_kernel_thread(uint32_t ordinal)
{
    switch (ordinal) {
    case 97u: return bridge_ke_cancel_timer;
    case 99u: return bridge_ke_delay_execution_thread;
    case 113u: return bridge_ke_initialize_timer_ex;
    case 124u: return bridge_ke_query_base_priority_thread;
    case 128u: return bridge_ke_query_system_time;
    case 143u: return bridge_ke_set_base_priority_thread;
    case 144u: return bridge_ke_set_disable_boost_thread;
    case 145u: return bridge_ke_set_event;
    case 149u: return bridge_ke_set_timer;
    case 151u: return bridge_ke_stall_execution_processor;
    case 159u: return bridge_ke_wait_for_single_object;
    case 189u: return bridge_nt_create_event;
    case 224u: return bridge_nt_resume_thread;
    case 225u: return bridge_nt_set_event;
    case 231u: return bridge_nt_suspend_thread;
    case 233u: return bridge_nt_wait_for_single_object;
    case 234u: return bridge_nt_wait_for_single_object_ex;
    case 238u: return bridge_nt_yield_execution;
    case 246u: return bridge_ob_reference_object_by_handle;
    case 250u: return bridge_obf_dereference_object;
    default: return NULL;
    }
}
