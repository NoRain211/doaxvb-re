#include "kernel_abi.h"
#include "program_manual.h"
#include "runtime.h"
#include "stop_report.h"
#include "xbox_memory_layout.h"

#include <inttypes.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

uint32_t g_recomp_178bb0_origin;
uint32_t g_recomp_186fb5_origin;
uint32_t g_recomp_17f8e0_origin;
uint32_t g_recomp_78d80_origin;
uint32_t g_recomp_7c450_origin;
uint32_t g_recomp_7c7c0_origin;

enum {
    XBOX_CACHED_ALIAS = 0x80000000u,
    XBOX_HEAP_BASE = 0x01000000u,
    XBOX_HEAP_LIMIT = 0x03800000u,
    XBOX_CONTIGUOUS_LIMIT = 0x03f00000u,
    XBOX_PAGE_SIZE = 0x1000u,
};

static uint32_t heap_cursor = XBOX_HEAP_BASE;
static uint32_t contiguous_cursor = XBOX_CONTIGUOUS_LIMIT;
static uint32_t next_thread_handle = 0xbee10000u;
static uint32_t current_thread_id = 1u;
static jmp_buf thread_exit;
static int thread_exit_active;

extern RecompFunction recomp_lookup(uint32_t guest_address);

static void push32(uint32_t value)
{
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = value;
}

static void call0(uint32_t guest_address)
{
    uint32_t saved_esp = recomp_runtime.registers.esp;

    push32(0u);
    recomp_dispatch_indirect_site(
        guest_address, saved_esp, __FILE__, __LINE__);
}

static void call3(
    uint32_t guest_address,
    uint32_t first,
    uint32_t second,
    uint32_t third)
{
    uint32_t saved_esp = recomp_runtime.registers.esp;

    push32(third);
    push32(second);
    push32(first);
    push32(0u);
    recomp_dispatch_indirect_site(
        guest_address, saved_esp, __FILE__, __LINE__);
}

void recomp_program_missing(uint32_t guest_address)
{
    fprintf(
        stderr,
        "recomp program: missing generated body at 0x%08" PRIx32 "\n",
        guest_address);
    recomp_stop(2, "missing-body:0x%08" PRIx32, guest_address);
}

static void bridge_ps_create_system_thread_ex(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t handle_pointer = *recomp_memory_u32(entry_esp + 4u);
    uint32_t thread_id_pointer = *recomp_memory_u32(entry_esp + 20u);
    uint32_t start_context1 = *recomp_memory_u32(entry_esp + 24u);
    uint32_t start_context2 = *recomp_memory_u32(entry_esp + 28u);
    uint32_t create_suspended = *recomp_memory_u32(entry_esp + 32u);
    uint32_t start_routine = *recomp_memory_u32(entry_esp + 40u);
    uint32_t handle = next_thread_handle;

    next_thread_handle += 4u;
    if (handle_pointer != 0u) {
        *recomp_memory_u32(handle_pointer) = handle;
    }
    if (thread_id_pointer != 0u) {
        *recomp_memory_u32(thread_id_pointer) = handle;
    }

    fprintf(
        stderr,
        "recomp kernel: PsCreateSystemThreadEx start=0x%08" PRIx32
        " context1=0x%08" PRIx32 " context2=0x%08" PRIx32 "\n",
        start_routine,
        start_context1,
        start_context2);

    if (start_routine != 0u && create_suspended == 0u) {
        RecompRegisters creator = recomp_runtime.registers;
        uint32_t creator_thread_id = current_thread_id;

        recomp_runtime.registers = (RecompRegisters){0};
        recomp_runtime.registers.esp = XBOX_STARTUP_THREAD_STACK_SLOT;
        current_thread_id = handle;
        push32(start_context2);
        push32(start_context1);
        push32(0u);
        thread_exit_active = 1;
        if (setjmp(thread_exit) == 0) {
            recomp_dispatch_indirect_site(
                start_routine,
                recomp_runtime.registers.esp + 12u,
                __FILE__,
                __LINE__);
        }
        thread_exit_active = 0;
        current_thread_id = creator_thread_id;
        recomp_runtime.registers = creator;
    }

    recomp_runtime.registers.esp = entry_esp + 44u;
    recomp_runtime.registers.eax = 0u;
}

static void bridge_ps_terminate_system_thread(void)
{
    uint32_t exit_status = *recomp_memory_u32(
        recomp_runtime.registers.esp + 4u);

    fprintf(
        stderr,
        "recomp kernel: PsTerminateSystemThread status=0x%08" PRIx32 "\n",
        exit_status);
    if (!thread_exit_active) {
        fprintf(stderr, "recomp kernel: thread termination outside a thread\n");
        recomp_stop(2, "thread-outside");
    }
    longjmp(thread_exit, 1);
}

static void bridge_rtl_initialize_critical_section(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t critical_section = *recomp_memory_u32(entry_esp + 4u);

    if (critical_section == 0u) {
        recomp_runtime.registers.eax = 0xc0000001u;
    } else {
        *recomp_memory_u32(critical_section) = 0x00000401u;
        *recomp_memory_u32(critical_section + 4u) = 0u;
        *recomp_memory_u32(critical_section + 8u) = critical_section + 8u;
        *recomp_memory_u32(critical_section + 12u) = critical_section + 8u;
        *recomp_memory_u32(critical_section + 16u) = 0xffffffffu;
        *recomp_memory_u32(critical_section + 20u) = 0u;
        *recomp_memory_u32(critical_section + 24u) = 0u;
        recomp_runtime.registers.eax = 0u;
    }
    recomp_runtime.registers.esp = entry_esp + 8u;
}

static void bridge_rtl_enter_critical_section(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t critical_section = *recomp_memory_u32(entry_esp + 4u);
    uint32_t owner;
    uint32_t recursion;
    uint32_t lock_count;

    /* The adapter dereferences its argument before anything else, so a bad
       pointer faults with no context. Report it against the argument slot. */
    if (critical_section == 0u || critical_section >= 0x04000000u) {
        fprintf(
            stderr,
            "recomp kernel: RtlEnterCriticalSection pointer 0x%08" PRIx32
            " out of guest RAM entry_esp=0x%08" PRIx32 "\n",
            critical_section,
            entry_esp);
    }

    owner = *recomp_memory_u32(critical_section + 24u);
    recursion = *recomp_memory_u32(critical_section + 20u);
    lock_count = *recomp_memory_u32(critical_section + 16u);

    if (owner != 0u && owner != current_thread_id) {
        fprintf(
            stderr,
            "recomp kernel: contended critical section 0x%08" PRIx32
            " owner=0x%08" PRIx32 " thread=0x%08" PRIx32 "\n",
            critical_section,
            owner,
            current_thread_id);
        recomp_stop(2, "critical-section:0x%08" PRIx32, critical_section);
    }

    *recomp_memory_u32(critical_section + 4u) = 0u;
    *recomp_memory_u32(critical_section + 16u) =
        owner == current_thread_id ? lock_count + 1u : 0u;
    *recomp_memory_u32(critical_section + 20u) =
        owner == current_thread_id ? recursion + 1u : 1u;
    *recomp_memory_u32(critical_section + 24u) = current_thread_id;
    recomp_runtime.registers.esp = entry_esp + 8u;
    recomp_runtime.registers.eax = 0u;
}

static void bridge_rtl_leave_critical_section(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t critical_section = *recomp_memory_u32(entry_esp + 4u);
    uint32_t owner = *recomp_memory_u32(critical_section + 24u);
    uint32_t recursion = *recomp_memory_u32(critical_section + 20u);
    uint32_t lock_count = *recomp_memory_u32(critical_section + 16u);

    if (owner != current_thread_id || recursion == 0u) {
        fprintf(
            stderr,
            "recomp kernel: invalid critical-section release 0x%08" PRIx32
            " owner=0x%08" PRIx32 " thread=0x%08" PRIx32 "\n",
            critical_section,
            owner,
            current_thread_id);
        recomp_stop(2, "critical-section:0x%08" PRIx32, critical_section);
    }

    --recursion;
    *recomp_memory_u32(critical_section + 16u) = lock_count - 1u;
    *recomp_memory_u32(critical_section + 20u) = recursion;
    if (recursion == 0u) {
        *recomp_memory_u32(critical_section + 24u) = 0u;
    }
    recomp_runtime.registers.esp = entry_esp + 8u;
    recomp_runtime.registers.eax = 0u;
}

RecompFunction recomp_kernel_startup(uint32_t ordinal)
{
    switch (ordinal) {
    case 255u: return bridge_ps_create_system_thread_ex;
    case 258u: return bridge_ps_terminate_system_thread;
    case 277u: return bridge_rtl_enter_critical_section;
    case 291u: return bridge_rtl_initialize_critical_section;
    case 294u: return bridge_rtl_leave_critical_section;
    default: return NULL;
    }
}

RecompFunction recomp_program_lookup(uint32_t guest_address)
{
    /* Every indirect call resolves here, and the manual chain is long. Manual
       and generated results are fixed once their environment reads are done.
       Kernel thunks stay uncached: one is time-dependent and the kernel
       lookup records each call. */
    static _Thread_local struct {
        uint32_t address;
        RecompFunction function;
    } cache[4096];
    const bool kernel = (guest_address & 0xffff0000u) == 0x80000000u;
    const size_t slot = (guest_address >> 2 ^ guest_address >> 14) & 4095u;
    if (!kernel && cache[slot].function != NULL && cache[slot].address == guest_address)
        return cache[slot].function;
    RecompFunction function = recomp_lookup_manual(guest_address);

    if (function == NULL) {
        function = recomp_lookup(guest_address);
    }
    if (function != NULL && !kernel) {
        cache[slot].address = guest_address;
        cache[slot].function = function;
    }
    if (function == NULL) {
        function = recomp_lookup_kernel(guest_address);
    }
    return function;
}

void recomp_program_thread_start(void)
{
    uint32_t thread_state;

    call0(0x00186c29u);
    call0(0x001864eau);

    thread_state = *recomp_memory_u32(0x20u);
    thread_state = *recomp_memory_u32(thread_state + 0x250u);
    thread_state = thread_state == 0u
        ? 0u
        : *recomp_memory_u32(thread_state + 0x24u);
    if (thread_state != 0u) {
        uint32_t saved_edi = recomp_runtime.registers.edi;
        uint32_t tls = *recomp_memory_u32(0x28u);
        uint32_t tls_slots = *recomp_memory_u32(4u);
        uint32_t slot = *recomp_memory_u32(0x003b5258u);
        uint32_t value = *recomp_memory_u32(tls_slots + slot * 4u);

        value -= *recomp_memory_u32(tls + 0x28u);
        *recomp_memory_i8(thread_state) = 1;
        *recomp_memory_u32(thread_state + 4u) = value + 0x18u;
        recomp_runtime.registers.edi = saved_edi;
    }

    call0(0x00186c00u);
    call0(0x00186ba8u);
    /* Default to Digital; loading saved settings still restores the chosen mode. */
    *recomp_memory_i8(0x003a4c38u) = 0;
    call3(0x000bb290u, 0u, 0u, 0u);
    recomp_runtime.registers.esp += 12u;
    call3(0x00186396u, 1u, 1u, 0u);
    recomp_runtime.registers.eax = 0u;
    recomp_runtime.registers.esp += 8u;
}

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint64_t aligned;
    uint64_t end;

    if (alignment < 4u || (alignment & (alignment - 1u)) != 0u) {
        return 0u;
    }
    aligned = ((uint64_t)heap_cursor + alignment - 1u) & ~(uint64_t)(alignment - 1u);
    end = aligned + size;
    if (end > XBOX_HEAP_LIMIT || end > contiguous_cursor) {
        return 0u;
    }
    heap_cursor = (uint32_t)end;
    return (uint32_t)aligned;
}

void xbox_HeapFree(uint32_t guest_address)
{
    (void)guest_address;
}

uint32_t xbox_HeapCheckpoint(void)
{
    return heap_cursor;
}

bool xbox_HeapRestore(uint32_t checkpoint)
{
    if (checkpoint < XBOX_HEAP_BASE || checkpoint > heap_cursor) {
        return false;
    }
    heap_cursor = checkpoint;
    return true;
}

uint32_t xbox_ContiguousAlloc(
    uint32_t size,
    uint32_t lowest_address,
    uint32_t highest_address,
    uint32_t alignment)
{
    uint64_t rounded_size;
    uint64_t upper_bound;
    uint64_t base;

    if (size == 0u) {
        return 0u;
    }
    if (alignment < XBOX_PAGE_SIZE) {
        alignment = XBOX_PAGE_SIZE;
    }
    if ((alignment & (alignment - 1u)) != 0u) {
        return 0u;
    }
    rounded_size = ((uint64_t)size + XBOX_PAGE_SIZE - 1u) &
        ~(uint64_t)(XBOX_PAGE_SIZE - 1u);
    upper_bound = highest_address == UINT32_MAX
        ? XBOX_CONTIGUOUS_LIMIT
        : (uint64_t)highest_address + 1u;
    if (upper_bound > contiguous_cursor) {
        upper_bound = contiguous_cursor;
    }
    if (rounded_size > upper_bound) {
        return 0u;
    }
    base = (upper_bound - rounded_size) & ~(uint64_t)(alignment - 1u);
    if (base < lowest_address || base < heap_cursor) {
        return 0u;
    }
    contiguous_cursor = (uint32_t)base;
    return XBOX_CACHED_ALIAS | (uint32_t)base;
}

uint64_t doaxbv_ftol2_i64_bits(double value)
{
    if (value != value) {
        return 0u;
    }
    if (value >= 9223372036854775808.0) {
        return (uint64_t)INT64_MAX;
    }
    if (value <= -9223372036854775808.0) {
        return (uint64_t)INT64_MIN;
    }
    return (uint64_t)(int64_t)value;
}

int doaxbv_a241d4_watch_begin(const char *label)
{
    (void)label;
    return 0;
}

void doaxbv_a241d4_watch_end(const char *label, int started)
{
    (void)label;
    (void)started;
}

void doaxbv_trace_7dc50_stage(const char *stage, uint32_t ebp_value)
{
    (void)stage;
    (void)ebp_value;
}

void doaxbv_trace_7a1d0_frame(const char *stage, uint32_t ebp_value)
{
    (void)stage;
    (void)ebp_value;
}

void doaxbv_trace_a2db0_callsite(
    uint32_t callsite,
    uint32_t esp_value,
    uint32_t esi_value,
    uint32_t edi_value,
    uint32_t arg_value)
{
    (void)callsite;
    (void)esp_value;
    (void)esi_value;
    (void)edi_value;
    (void)arg_value;
}

void doaxbv_trace_a3060_sort(
    const char *stage,
    uint32_t esi_value,
    uint32_t ecx_value,
    uint32_t eax_value,
    uint32_t edx_value)
{
    (void)stage;
    (void)esi_value;
    (void)ecx_value;
    (void)eax_value;
    (void)edx_value;
}

void doaxbv_trace_17f940_state(
    const char *stage,
    uint32_t source_arg,
    uint32_t manager,
    uint32_t result,
    uint32_t esp_value)
{
    (void)stage;
    (void)source_arg;
    (void)manager;
    (void)result;
    (void)esp_value;
}

void doaxbv_trace_17fae0_call(
    uint32_t manager,
    uint32_t source_base,
    uint32_t list_cursor,
    uint32_t index,
    uint32_t raw_offset,
    uint32_t descriptor,
    uint32_t out_slot,
    uint32_t count)
{
    (void)manager;
    (void)source_base;
    (void)list_cursor;
    (void)index;
    (void)raw_offset;
    (void)descriptor;
    (void)out_slot;
    (void)count;
}

void doaxbv_trace_17eb50_stage(
    uint32_t stage,
    uint32_t esp_value,
    uint32_t arg,
    uint32_t cursor)
{
    (void)stage;
    (void)esp_value;
    (void)arg;
    (void)cursor;
}

void doaxbv_trace_187960_metadata_fail(
    uint32_t request,
    uint32_t archive_slot,
    uint32_t entry_index,
    uint32_t path_buf,
    uint32_t status)
{
    (void)request;
    (void)archive_slot;
    (void)entry_index;
    (void)path_buf;
    (void)status;
}

void doaxbv_ensure_raw_device_command_write(uint32_t stage)
{
    (void)stage;
    recomp_program_missing(0x001e4d80u);
}

void doaxbv_trace_raw_submit_pending(
    uint32_t device,
    uint32_t entry,
    uint32_t pending,
    uint32_t stage)
{
    (void)device;
    (void)entry;
    (void)pending;
    (void)stage;
}

void doaxbv_raw_clear_kickoff_wait(
    uint32_t command_state,
    uint32_t original_va)
{
    (void)command_state;
    recomp_program_missing(original_va);
}

void doaxbv_raw_advance_get_pointer_wait(
    uint32_t get_ptr,
    uint32_t put,
    uint32_t target_delta)
{
    (void)get_ptr;
    (void)put;
    (void)target_delta;
    recomp_program_missing(0x001ea050u);
}
