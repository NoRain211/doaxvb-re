#include "fiber_adapter.h"
#include "cri_service_adapter.h"
#include "d3d_frame_adapter.h"
#include "stop_report.h"
#include "xbox_memory_layout.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "win32_compat.h"
#include <stdlib.h>
#include <sys/mman.h>

void recomp_fiber_switch(void **from_sp, void *to_sp);
void recomp_fiber_trampoline(void);

typedef struct RecompNativeFiber {
    void *sp;
    void *stack_memory;
    size_t stack_size;
} RecompNativeFiber;

static RecompNativeFiber *g_active_native_fiber = NULL;

#define FIBER_FLAG_FLOAT_SWITCH 0x1

static LPVOID ConvertThreadToFiberEx(LPVOID lpParameter, DWORD dwFlags)
{
    (void)lpParameter;
    (void)dwFlags;
    RecompNativeFiber *fiber = (RecompNativeFiber *)calloc(1, sizeof(RecompNativeFiber));
    g_active_native_fiber = fiber;
    return (LPVOID)fiber;
}

static BOOL ConvertFiberToThread(void)
{
    if (g_active_native_fiber) {
        free(g_active_native_fiber);
        g_active_native_fiber = NULL;
    }
    return TRUE;
}

static LPVOID CreateFiberEx(
    SIZE_T dwStackCommit,
    SIZE_T dwStackReserve,
    DWORD dwFlags,
    void (*lpStartAddress)(void *),
    LPVOID lpParameter)
{
    (void)dwStackCommit;
    (void)dwFlags;
    size_t stack_size = dwStackReserve ? dwStackReserve : 1024 * 1024;
    stack_size = (stack_size + 0xfffu) & ~0xfffu;
    void *stack_mem = mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stack_mem == MAP_FAILED) {
        return NULL;
    }

    RecompNativeFiber *fiber = (RecompNativeFiber *)calloc(1, sizeof(RecompNativeFiber));
    if (!fiber) {
        munmap(stack_mem, stack_size);
        return NULL;
    }
    fiber->stack_memory = stack_mem;
    fiber->stack_size = stack_size;

    uintptr_t top = ((uintptr_t)stack_mem + stack_size) & ~15ULL;
#if defined(__arm64__) || defined(__aarch64__)
    top -= 160;
    uint64_t *frame = (uint64_t *)top;
    frame[8] = 0; // fp
    frame[9] = (uint64_t)(uintptr_t)recomp_fiber_trampoline; // lr
    frame[18] = (uint64_t)(uintptr_t)lpParameter; // x19
    frame[19] = (uint64_t)(uintptr_t)lpStartAddress; // x20
    fiber->sp = (void *)top;
#elif defined(__x86_64__) || defined(_M_X64)
    top -= 64;
    uint64_t *frame = (uint64_t *)top;
    frame[0] = 0; // r15
    frame[1] = 0; // r14
    frame[2] = (uint64_t)(uintptr_t)lpStartAddress; // r13
    frame[3] = (uint64_t)(uintptr_t)lpParameter; // r12
    frame[4] = 0; // rbx
    frame[5] = 0; // rbp
    frame[6] = (uint64_t)(uintptr_t)recomp_fiber_trampoline; // ret rip
    frame[7] = 0; // 16-byte alignment padding
    fiber->sp = (void *)top;
#else
#error "Unsupported architecture for native POSIX fiber switching"
#endif

    return (LPVOID)fiber;
}

static void SwitchToFiber(LPVOID lpFiber)
{
    RecompNativeFiber *target = (RecompNativeFiber *)lpFiber;
    RecompNativeFiber *from = g_active_native_fiber;
    g_active_native_fiber = target;
    recomp_fiber_switch(&from->sp, target->sp);
}

static void DeleteFiber(LPVOID lpFiber)
{
    if (!lpFiber) return;
    RecompNativeFiber *fiber = (RecompNativeFiber *)lpFiber;
    if (fiber->stack_memory) {
        munmap(fiber->stack_memory, fiber->stack_size);
    }
    if (g_active_native_fiber == fiber) {
        g_active_native_fiber = NULL;
    }
    free(fiber);
}
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    XAPI_CREATE_FIBER_ADDRESS = 0x00182fbbu,
    XAPI_DELETE_FIBER_ADDRESS = 0x00183047u,
    XAPI_SWITCH_TO_FIBER_ADDRESS = 0x0018305au,
    XAPI_CONVERT_THREAD_TO_FIBER_ADDRESS = 0x00183099u,
    XAPI_MINIMUM_FIBER_STACK = 0x3000u,
    NATIVE_FIBER_STACK_RESERVE = 0x100000u,
};

typedef struct RecompHostFiber {
    bool active;
    uint32_t guest_handle;
    uint32_t guest_stack_base;
    uint32_t guest_stack_size;
    uint32_t entry;
    uint64_t switch_count;
    LPVOID native_fiber;
} RecompHostFiber;

static RecompFiberModel fiber_model;
static RecompHostFiber host_fibers[RECOMP_FIBER_MAX_COUNT];
static RecompHostFiber *current_host_fiber;
static bool converted_thread;

static uint32_t main_fiber_handle(void)
{
    uint32_t tls_index = *recomp_memory_u32(0x003b5258u);
    uint32_t tls_slots = *recomp_memory_u32(4u);
    uint32_t tls_block = *recomp_memory_u32(tls_slots + tls_index * 4u);

    return tls_block + 8u;
}

static void publish_current_fiber(uint32_t guest_handle)
{
    uint32_t tls_index = *recomp_memory_u32(0x003b5258u);
    uint32_t tls_slots = *recomp_memory_u32(4u);
    uint32_t tls_block = *recomp_memory_u32(tls_slots + tls_index * 4u);

    *recomp_memory_u32(tls_block + 4u) = guest_handle;
}

static uint32_t stack_argument(uint32_t entry_esp, uint32_t index)
{
    return *recomp_memory_u32(entry_esp + 4u + index * 4u);
}

static void finish(uint32_t entry_esp, uint32_t argument_count, uint32_t result)
{
    recomp_runtime.registers.eax = result;
    recomp_runtime.registers.esp =
        entry_esp + 4u + argument_count * 4u;
}

static void fail_fiber(const char *reason, uint32_t guest_handle)
{
    fprintf(
        stderr,
        "recomp fiber: %s 0x%08" PRIx32 "\n",
        reason,
        guest_handle);
    recomp_stop(2, "fiber:%s:0x%08" PRIx32, reason, guest_handle);
}

static RecompHostFiber *host_fiber_find(uint32_t guest_handle)
{
    for (size_t i = 0u; i < RECOMP_FIBER_MAX_COUNT; ++i) {
        if (host_fibers[i].active &&
            host_fibers[i].guest_handle == guest_handle) {
            return &host_fibers[i];
        }
    }
    return NULL;
}

static RecompHostFiber *host_fiber_add(
    uint32_t guest_handle,
    LPVOID native_fiber)
{
    if (native_fiber == NULL || host_fiber_find(guest_handle) != NULL) {
        return NULL;
    }
    for (size_t i = 0u; i < RECOMP_FIBER_MAX_COUNT; ++i) {
        if (!host_fibers[i].active) {
            host_fibers[i] = (RecompHostFiber){
                .active = true,
                .guest_handle = guest_handle,
                .native_fiber = native_fiber,
            };
            return &host_fibers[i];
        }
    }
    return NULL;
}

static VOID WINAPI fiber_entry(void *parameter)
{
    RecompHostFiber *host = parameter;
    RecompFiber *fiber = recomp_fiber_find(&fiber_model, host->guest_handle);
    uint32_t entry;

    if (fiber == NULL || current_host_fiber != host ||
        fiber_model.current_handle != host->guest_handle) {
        fail_fiber("entry-context", host->guest_handle);
    }
    entry = fiber->entry;
    recomp_runtime.registers = fiber->registers;
    *recomp_memory_u32(0u) = fiber->exception_list;
    recomp_dispatch_indirect_site(
        entry, fiber->registers.esp, __FILE__, __LINE__);
    fail_fiber("entry-returned", host->guest_handle);
}

void recomp_fiber_adapter_reset(void)
{
    if (converted_thread && current_host_fiber != NULL &&
        current_host_fiber->guest_handle == main_fiber_handle()) {
        for (size_t i = 0u; i < RECOMP_FIBER_MAX_COUNT; ++i) {
            RecompHostFiber *host = &host_fibers[i];

            if (host->active && host != current_host_fiber) {
                DeleteFiber(host->native_fiber);
            }
        }
        (void)ConvertFiberToThread();
    }
    recomp_fiber_model_reset(&fiber_model);
    for (size_t i = 0u; i < RECOMP_FIBER_MAX_COUNT; ++i) {
        host_fibers[i] = (RecompHostFiber){0};
    }
    current_host_fiber = NULL;
    converted_thread = false;
}

const RecompFiberModel *recomp_fiber_adapter_model(void)
{
    return &fiber_model;
}

void recomp_fiber_adapter_report(void)
{
    for (size_t i = 0u; i < RECOMP_FIBER_MAX_COUNT; ++i) {
        const RecompHostFiber *host = &host_fibers[i];

        if (!host->active) {
            continue;
        }
        fprintf(
            stderr,
            "recomp fiber: handle=0x%08" PRIx32 " entry=0x%08" PRIx32
            " resumed=%llu%s\n",
            host->guest_handle,
            host->entry,
            (unsigned long long)host->switch_count,
            host == current_host_fiber ? " (current)" : "");
    }
}

static void convert_thread_to_fiber_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t fiber_data = stack_argument(entry_esp, 0u);
    uint32_t guest_handle = main_fiber_handle();
    RecompRegisters registers = recomp_runtime.registers;
    LPVOID native_fiber;
    RecompFiber *fiber;
    RecompHostFiber *host;

    if (converted_thread || current_host_fiber != NULL) {
        fail_fiber("duplicate-convert", guest_handle);
    }
    native_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    if (native_fiber == NULL) {
        fail_fiber("convert-failed", guest_handle);
    }
    fiber = recomp_fiber_add(
        &fiber_model,
        guest_handle,
        0u,
        fiber_data,
        &registers,
        *recomp_memory_u32(0u));
    host = host_fiber_add(guest_handle, native_fiber);
    if (fiber == NULL || host == NULL ||
        !recomp_fiber_set_current(&fiber_model, guest_handle)) {
        fail_fiber("convert-state", guest_handle);
    }

    *recomp_memory_u32(guest_handle) = fiber_data;
    *recomp_memory_u32(guest_handle + 4u) = entry_esp + 8u;
    *recomp_memory_u32(guest_handle + 8u) = 0u;
    *recomp_memory_u32(guest_handle + 12u) = entry_esp;
    publish_current_fiber(guest_handle);
    converted_thread = true;
    current_host_fiber = host;
    finish(entry_esp, 1u, guest_handle);
}

static void create_fiber_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t stack_size = stack_argument(entry_esp, 0u);
    uint32_t entry = stack_argument(entry_esp, 1u);
    uint32_t parameter = stack_argument(entry_esp, 2u);
    uint32_t stack_base;
    uint32_t stack_top;
    uint32_t guest_handle;
    uint32_t initial_esp;
    RecompRegisters registers = {0};
    RecompFiber *fiber;
    RecompHostFiber *host;
    LPVOID native_fiber;

    if (!converted_thread || entry == 0u) {
        fail_fiber("create-context", entry);
    }
    if (stack_size < XAPI_MINIMUM_FIBER_STACK) {
        stack_size = XAPI_MINIMUM_FIBER_STACK;
    }
    stack_size = (stack_size + 0xfffu) & 0xfffff000u;
    host = NULL;
    for (size_t i = 0u; i < RECOMP_FIBER_MAX_COUNT; ++i) {
        if (!host_fibers[i].active) {
            host = &host_fibers[i];
            break;
        }
    }
    if (host == NULL) {
        fail_fiber("create-host-capacity", entry);
    }
    if (host->guest_stack_size >= stack_size) {
        stack_base = host->guest_stack_base;
        stack_size = host->guest_stack_size;
    } else {
        stack_base = xbox_HeapAlloc(stack_size, 0x1000u);
    }
    if (stack_base == 0u || stack_base > UINT32_MAX - stack_size) {
        fail_fiber("guest-stack", entry);
    }
    recomp_guest_memset(stack_base, 0, stack_size);
    stack_top = stack_base + stack_size;
    guest_handle = stack_top - 0x10u;
    initial_esp = guest_handle - 8u;
    registers.esp = initial_esp;

    *recomp_memory_u32(initial_esp) = 0u;
    *recomp_memory_u32(initial_esp + 4u) = parameter;
    *recomp_memory_u32(guest_handle) = parameter;
    *recomp_memory_u32(guest_handle + 4u) = stack_top;
    *recomp_memory_u32(guest_handle + 8u) = stack_base;
    *recomp_memory_u32(guest_handle + 12u) = initial_esp;

    fiber = recomp_fiber_add(
        &fiber_model,
        guest_handle,
        entry,
        parameter,
        &registers,
        0xffffffffu);
    if (fiber == NULL) {
        fail_fiber("create-state", guest_handle);
    }
    native_fiber = CreateFiberEx(
        0u,
        NATIVE_FIBER_STACK_RESERVE,
        FIBER_FLAG_FLOAT_SWITCH,
        fiber_entry,
        host);
    if (native_fiber == NULL) {
        fail_fiber("create-host", guest_handle);
    }
    *host = (RecompHostFiber){
        .active = true,
        .guest_handle = guest_handle,
        .guest_stack_base = stack_base,
        .guest_stack_size = stack_size,
        .entry = entry,
        .native_fiber = native_fiber,
    };
    fprintf(
        stderr,
        "recomp fiber: created handle=0x%08" PRIx32 " entry=0x%08" PRIx32
        " parameter=0x%08" PRIx32 "\n",
        guest_handle,
        entry,
        parameter);
    finish(entry_esp, 3u, guest_handle);
}

static void switch_to_fiber_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t result = recomp_runtime.registers.eax;
    uint32_t target_handle = stack_argument(entry_esp, 0u);
    RecompFiber *outgoing = recomp_fiber_current(&fiber_model);
    RecompFiber *target = recomp_fiber_find(&fiber_model, target_handle);
    RecompHostFiber *outgoing_host = current_host_fiber;
    RecompHostFiber *target_host = host_fiber_find(target_handle);

    if (outgoing == NULL || target == NULL || outgoing_host == NULL ||
        target_host == NULL) {
        fail_fiber("unknown-switch", target_handle);
    }
    /* Round 119 probe. Screen 7 (the island map) reaches a cooperative wait in
       guest sub_000DEC50 at loc_000DED70 that yields here every iteration and
       never exits: measured 99% of one core on this fiber with every other
       thread idle. Three conditions could keep it there, and they are
       distinguished entirely by state readable at this seam:
         1. the yield is a self-switch, so no other fiber can ever run;
         2. no entry in the 256-entry table at 0x5DDE84 matches the waited key;
         3. an entry matches but its completion word at +8 stays zero.
       Opt-in via RECOMP_SPINPROBE so the default run is unchanged. */
    {
        static const char *spin_probe;
        static bool spin_probe_read;
        static uint32_t spin_lines;
        static uint32_t spin_switches;
        static uint32_t spin_self;

        if (!spin_probe_read) {
            spin_probe_read = true;
            spin_probe = getenv("RECOMP_SPINPROBE");
        }
        if (spin_probe != NULL) {
            ++spin_switches;
            if (target_handle == outgoing->guest_handle) {
                ++spin_self;
            }
            /* Report sparsely: the spin issues these continuously, so a line
               per switch would bury the log. */
            if (spin_lines < 400u && (spin_switches % 20000u) == 0u) {
                uint32_t record_root = *recomp_memory_u32(0x00317764u);
                uint32_t record = *recomp_memory_u32(record_root);
                uint32_t want_handle = *recomp_memory_u32(record + 0x134u);
                uint32_t sel_index = *recomp_memory_u16(0x0031E4FCu);
                uint32_t sel_table = *recomp_memory_u32(0x0031E504u);
                uint32_t want_key =
                    *recomp_memory_u32(sel_table + sel_index * 4u) & 0xFFFFu;
                uint32_t match_slot = 0xFFFFFFFFu;
                uint32_t match_done = 0u;
                uint32_t key_only_slot = 0xFFFFFFFFu;
                uint32_t nonzero_done = 0u;
                uint32_t free_slots = 0u;
                uint32_t zero_handle_slots = 0u;
                uint32_t i;
                /* sub_000A2E99..sub_000A2ECE count the ADXF channels whose
                   handle word (0x005DEC58 + ch*0x14 + 0x10) is non-NULL and
                   refuse to start any new request once that count reaches the
                   limit byte at 0x005DD882. One channel left open forever
                   therefore starves every later load. Report all five. */
                uint32_t chan_open = 0u;
                uint32_t staged_slots = 0u;
                uint32_t chan[5];

                for (i = 0u; i < 5u; ++i) {
                    chan[i] = *recomp_memory_u32(0x005DEC68u + i * 0x14u);
                    if (chan[i] != 0u) {
                        ++chan_open;
                    }
                }
                /* Queue slots live at 0x005DD884 + n*0xC with the state byte
                   at +1; 0xFE means staged and waiting for a channel. */
                for (i = 0u; i < 0x80u; ++i) {
                    if ((*recomp_memory_i8(0x005DD885u + i * 0xCu) & 0xFF) ==
                        0xFE) {
                        ++staged_slots;
                    }
                }

                for (i = 0u; i < 0x100u; ++i) {
                    uint32_t entry = 0x005DDE84u + i * 0xCu;
                    uint32_t key = *recomp_memory_u16(entry);
                    uint32_t handle = *recomp_memory_u32(entry + 4u);
                    uint32_t done = *recomp_memory_u32(entry + 8u);

                    if (done != 0u) {
                        ++nonzero_done;
                    }
                    /* sub_000A3140 publishes into the first slot whose handle
                       word is 0xFFFFFFFF and silently drops the request when
                       none is free, so occupancy separates "never posted"
                       from "posted but the table was full". */
                    if (handle == 0xFFFFFFFFu) {
                        ++free_slots;
                    }
                    if (handle == 0u) {
                        ++zero_handle_slots;
                    }
                    if (key != want_key) {
                        continue;
                    }
                    if (key_only_slot == 0xFFFFFFFFu) {
                        key_only_slot = i;
                    }
                    if (handle == want_handle && match_slot == 0xFFFFFFFFu) {
                        match_slot = i;
                        match_done = done;
                    }
                }
                ++spin_lines;
                fprintf(
                    stderr,
                    "recomp spinprobe: switches=%" PRIu32
                    " self=%" PRIu32 " from=%08" PRIx32
                    " to=%08" PRIx32 " want_key=%04" PRIx32
                    " want_handle=%08" PRIx32 " match_slot=%" PRId32
                    " match_done=%08" PRIx32 " key_slot=%" PRId32
                    " table_done_count=%" PRIu32 "\n",
                    spin_switches,
                    spin_self,
                    outgoing->guest_handle,
                    target_handle,
                    want_key,
                    want_handle,
                    (int32_t)match_slot,
                    match_done,
                    (int32_t)key_only_slot,
                    nonzero_done);
                /* The request the spin waits on is posted into the mailbox at
                   0x005DDE78 and drained by sub_000A2E00 into the 128-entry
                   queue at 0x005DD885, which only publishes a table entry once
                   sub_000A32F0 polls the owning ADXF channel. Report the
                   mailbox and queue head next to the table result so a request
                   that was never posted is distinguishable from one posted but
                   never drained. */
                fprintf(
                    stderr,
                    "recomp spinq: switches=%" PRIu32
                    " staged=%02" PRIx32 " op=%02" PRIx32
                    " mbkey=%04" PRIx32 " mbarg=%08" PRIx32
                    " head=%02" PRIx32 " limit=%02" PRIx32
                    " active=%02" PRIx32 " ch0=%08" PRIx32
                    " ch1=%08" PRIx32 " stat=%" PRIu32
                    " wsteps=%" PRIu32 " free=%" PRIu32
                    " zerohnd=%" PRIu32 " chanopen=%" PRIu32
                    " staged_q=%" PRIu32 " ch=%08" PRIx32
                    ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32
                    ",%08" PRIx32 "\n",
                    spin_switches,
                    (uint32_t)(*recomp_memory_i8(0x005DDE79u) & 0xFF),
                    (uint32_t)(*recomp_memory_i8(0x005DDE78u) & 0xFF),
                    (uint32_t)*recomp_memory_u16(0x005DDE7Au),
                    *recomp_memory_u32(0x005DDE7Cu),
                    (uint32_t)(*recomp_memory_i8(0x005DD885u) & 0xFF),
                    (uint32_t)(*recomp_memory_i8(0x005DD882u) & 0xFF),
                    (uint32_t)(*recomp_memory_i8(0x005DD880u) & 0xFF),
                    *recomp_memory_u32(0x005DEC68u),
                    *recomp_memory_u32(0x005DEC7Cu),
                    recomp_cri_service_adxf_get_stat_calls(),
                    recomp_cri_service_file_worker_steps(),
                    free_slots,
                    zero_handle_slots,
                    chan_open,
                    staged_slots,
                    chan[0], chan[1], chan[2], chan[3], chan[4]);
            }
        }
    }
    /* Round 121 bridge. At screen 7 the guest waits in sub_000DEC50's scan at
       loc_000DED70 for a table entry matching (selector key, record handle).
       The request that would produce that entry is posted once, above the
       loop, and loc_000DED3C skips the post outright when the staging slot is
       busy at that instant; the loop below has no re-post path. Round 120
       measured the aftermath at the stall: nothing staged, no ADXF channel
       open, 223 of 256 table slots free and no slot carrying the waited key,
       so the scan can never be satisfied and the swap counter stops. Post the
       same request the guest would have posted, into queue entry 0 (where the
       insertion sort in sub_000A3060 would have placed it), and let the
       guest's own loader run it. No completion is fabricated: if the file is
       absent the entry still publishes with size zero and the wait stays put.
       Opt-in via RECOMP_REPOST so the default run is unchanged. */
    {
        static const char *repost_env;
        static bool repost_read;
        static uint32_t repost_count;
        static uint32_t switches_since_repost;

        if (!repost_read) {
            repost_read = true;
            repost_env = getenv("RECOMP_REPOST");
        }
        if (repost_env != NULL && repost_count < 8u) {
            static uint32_t last_swap;
            static uint32_t switches_at_swap;
            uint32_t swap_now = recomp_d3d_frame_adapter_swap_counter();

            ++switches_since_repost;
            /* Round 121 measured the first version of this gate firing during
               ordinary loading at swap 6408, where a request really was in
               flight, and that corrupted the walk. The stall is not "the wait
               loop is hot" -- that loop is hot during every normal load. The
               stall is the wait loop running while the swap counter has
               stopped advancing entirely. Require that: at least 50,000 fiber
               switches with no new frame presented. */
            if (swap_now != last_swap) {
                last_swap = swap_now;
                switches_at_swap = switches_since_repost;
            }
            if (switches_since_repost - switches_at_swap >= 50000u) {
                uint32_t record_root = *recomp_memory_u32(0x00317764u);
                uint32_t record = *recomp_memory_u32(record_root);
                uint32_t want_handle = *recomp_memory_u32(record + 0x134u);
                uint32_t sel_index = *recomp_memory_u16(0x0031E4FCu);
                uint32_t sel_table = *recomp_memory_u32(0x0031E504u);
                uint32_t want_key =
                    *recomp_memory_u32(sel_table + sel_index * 4u) & 0xFFFFu;
                bool matched = false;
                uint32_t i;

                switches_at_swap = switches_since_repost;
                for (i = 0u; i < 0x100u; ++i) {
                    uint32_t entry = 0x005DDE84u + i * 0xCu;

                    if ((uint32_t)*recomp_memory_u16(entry) == want_key &&
                        *recomp_memory_u32(entry + 4u) == want_handle) {
                        matched = true;
                        break;
                    }
                }
                /* Act only on the exact measured shape: a live wait with the
                   whole pipeline idle. Any other state has a request in
                   flight and must be left alone. */
                if (!matched && want_key != 0u && want_key != 0xFFFFu &&
                    (*recomp_memory_i8(0x005DDE79u) & 0xFF) != 0xFE &&
                    (*recomp_memory_i8(0x005DD885u) & 0xFF) == 0xFF) {
                    *recomp_memory_i8(0x005DD884u) = (int8_t)5;
                    *recomp_memory_u16(0x005DD886u) = (uint16_t)want_key;
                    *recomp_memory_u32(0x005DD888u) = want_handle;
                    *recomp_memory_i8(0x005DD885u) = (int8_t)0xFE;
                    ++repost_count;
                    fprintf(
                        stderr,
                        "recomp repost: n=%" PRIu32 " key=%04" PRIx32
                        " handle=%08" PRIx32 "\n",
                        repost_count,
                        want_key,
                        want_handle);
                }
            }
        }
    }
    if (target_handle == outgoing->guest_handle) {
        finish(entry_esp, 1u, result);
        return;
    }

    outgoing->registers = recomp_runtime.registers;
    outgoing->registers.esp = entry_esp + 8u;
    /* Hardware preserves the x87 and SSE context per thread. Measured at 240
       of 240 switches, this program's stack is non-empty at the boundary, so
       without this the incoming fiber inherits the outgoing one's floating
       point and silently computes with it. */
    recomp_fpu_context_save(&outgoing->fpu);
    /* Round 28 probe. RecompFiber carries only RecompRegisters, while the
       x87 stack, fpu_top and the XMM file live outside it in RecompRuntime
       and are therefore shared by every fiber. Real hardware gives each
       thread its own FP context. If fpu_top is ever non-zero here, live
       floating-point values are crossing a fiber boundary. Opt-in. */
    {
        static const char *fpu_trace;
        static bool fpu_trace_read;
        static uint32_t fpu_trace_lines;
        static uint32_t switches_seen;
        static uint32_t switches_with_stack;

        if (!fpu_trace_read) {
            fpu_trace_read = true;
            fpu_trace = getenv("RECOMP_FIBER_FPU");
        }
        if (fpu_trace != NULL) {
            ++switches_seen;
            if (recomp_runtime.fpu_top != 0u) {
                ++switches_with_stack;
            }
            if (fpu_trace_lines < 240u &&
                (recomp_runtime.fpu_top != 0u ||
                 (switches_seen % 500u) == 0u)) {
                ++fpu_trace_lines;
                fprintf(
                    stderr,
                    "recomp fiber fpu: switches=%" PRIu32
                    " nonempty=%" PRIu32 " top=%" PRIu32
                    " cmp=%d from=%08" PRIx32 " to=%08" PRIx32 "\n",
                    switches_seen,
                    switches_with_stack,
                    recomp_runtime.fpu_top,
                    recomp_runtime.fpu_compare,
                    outgoing->guest_handle,
                    target_handle);
            }
        }
    }
    ++target_host->switch_count;
    outgoing->exception_list = *recomp_memory_u32(0u);
    *recomp_memory_u32(outgoing->guest_handle + 12u) =
        outgoing->registers.esp;
    if (!recomp_fiber_set_current(&fiber_model, target_handle)) {
        fail_fiber("switch-state", target_handle);
    }
    recomp_runtime.registers = target->registers;
    recomp_fpu_context_restore(&target->fpu);
    *recomp_memory_u32(0u) = target->exception_list;
    publish_current_fiber(target_handle);
    current_host_fiber = target_host;
    SwitchToFiber(target_host->native_fiber);

    if (current_host_fiber != outgoing_host ||
        fiber_model.current_handle != outgoing->guest_handle) {
        fail_fiber("resume-context", outgoing->guest_handle);
    }
    recomp_runtime.registers.eax = result;
}

static void delete_fiber_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t guest_handle = stack_argument(entry_esp, 0u);
    RecompFiber *fiber = recomp_fiber_find(&fiber_model, guest_handle);
    RecompHostFiber *host = host_fiber_find(guest_handle);
    uint32_t stack_base;
    uint32_t stack_size;

    if (fiber == NULL || host == NULL || host == current_host_fiber) {
        fail_fiber("invalid-delete", guest_handle);
    }
    stack_base = host->guest_stack_base;
    stack_size = host->guest_stack_size;
    DeleteFiber(host->native_fiber);
    /* The guest allocator is monotonic. Keep one stack per host slot so a
       delete/create cycle reuses its address instead of exhausting the heap. */
    *host = (RecompHostFiber){
        .guest_stack_base = stack_base,
        .guest_stack_size = stack_size,
    };
    if (!recomp_fiber_remove(&fiber_model, guest_handle)) {
        fail_fiber("delete-state", guest_handle);
    }
    finish(entry_esp, 1u, 0u);
}

RecompFunction recomp_fiber_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case XAPI_CREATE_FIBER_ADDRESS:
        return create_fiber_adapter;
    case XAPI_DELETE_FIBER_ADDRESS:
        return delete_fiber_adapter;
    case XAPI_SWITCH_TO_FIBER_ADDRESS:
        return switch_to_fiber_adapter;
    case XAPI_CONVERT_THREAD_TO_FIBER_ADDRESS:
        return convert_thread_to_fiber_adapter;
    default:
        return NULL;
    }
}
