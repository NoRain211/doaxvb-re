#include "kernel_abi.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Interrupts and DPCs.
 *
 * Nothing here emulates an interrupt controller. DPCs are recorded and queued,
 * then run when IRQL drops below dispatch level, which is when the real kernel
 * runs them too. That is the whole model, and it is deliberate: the
 * native-host era stalled trying to raise real periodic interrupts.
 */

#define KDPC_TYPE 0x13u
#define KDPC_DEFERRED_ROUTINE 0x0cu
#define KDPC_DEFERRED_CONTEXT 0x10u
#define DISPATCH_LEVEL 2u
#define QUEUE_CAPACITY 64u

typedef struct QueuedDpc {
    uint32_t dpc;
    uint32_t argument1;
    uint32_t argument2;
} QueuedDpc;

static QueuedDpc dpc_queue[QUEUE_CAPACITY];
static unsigned dpc_queue_count;
static uint32_t current_irql;
static int draining;

typedef struct InterruptRecord {
    uint32_t object;
    uint32_t service_routine;
    uint32_t service_context;
    uint32_t vector;
} InterruptRecord;

static InterruptRecord interrupts[32];
static unsigned interrupt_count;

void recomp_kernel_drain_dpcs(void)
{
    if (draining) {
        return;
    }

    draining = 1;
    while (dpc_queue_count > 0u) {
        QueuedDpc queued = dpc_queue[0];
        uint32_t routine;
        uint32_t arguments[4];

        for (unsigned i = 1u; i < dpc_queue_count; ++i) {
            dpc_queue[i - 1u] = dpc_queue[i];
        }
        --dpc_queue_count;

        routine = *recomp_memory_u32(queued.dpc + KDPC_DEFERRED_ROUTINE);
        if (routine == 0u) {
            continue;
        }
        arguments[0] = queued.dpc;
        arguments[1] = *recomp_memory_u32(queued.dpc + KDPC_DEFERRED_CONTEXT);
        arguments[2] = queued.argument1;
        arguments[3] = queued.argument2;
        kernel_call_guest(routine, arguments, 4u);
    }
    draining = 0;
}

static void bridge_ke_initialize_dpc(void)
{
    uint32_t dpc = kernel_arg(1u);
    uint32_t routine = kernel_arg(2u);
    uint32_t context = kernel_arg(3u);

    if (dpc != 0u) {
        *recomp_memory_u32(dpc) = KDPC_TYPE;
        *recomp_memory_u32(dpc + 4u) = 0u;
        *recomp_memory_u32(dpc + 8u) = 0u;
        *recomp_memory_u32(dpc + KDPC_DEFERRED_ROUTINE) = routine;
        *recomp_memory_u32(dpc + KDPC_DEFERRED_CONTEXT) = context;
        *recomp_memory_u32(dpc + 0x14u) = 0u;
        *recomp_memory_u32(dpc + 0x18u) = 0u;
    }
    kernel_return(3u, 0u);
}

uint32_t recomp_kernel_queue_dpc(
    uint32_t dpc,
    uint32_t argument1,
    uint32_t argument2)
{
    uint32_t inserted = 0u;

    if (dpc != 0u) {
        int already_queued = 0;

        for (unsigned i = 0u; i < dpc_queue_count; ++i) {
            if (dpc_queue[i].dpc == dpc) {
                already_queued = 1;
                break;
            }
        }
        if (!already_queued) {
            if (dpc_queue_count >= QUEUE_CAPACITY) {
                fprintf(stderr, "recomp kernel: DPC queue overflow\n");
            } else {
                dpc_queue[dpc_queue_count].dpc = dpc;
                dpc_queue[dpc_queue_count].argument1 = argument1;
                dpc_queue[dpc_queue_count].argument2 = argument2;
                ++dpc_queue_count;
                inserted = 1u;
            }
        }
    }
    return inserted;
}

uint32_t recomp_kernel_remove_dpc(uint32_t dpc)
{
    uint32_t removed = 0u;

    for (unsigned i = 0u; i < dpc_queue_count; ++i) {
        if (dpc_queue[i].dpc != dpc) {
            continue;
        }
        for (unsigned j = i + 1u; j < dpc_queue_count; ++j) {
            dpc_queue[j - 1u] = dpc_queue[j];
        }
        --dpc_queue_count;
        removed = 1u;
        break;
    }
    return removed;
}

static void bridge_ke_insert_queue_dpc(void)
{
    kernel_return(
        3u,
        recomp_kernel_queue_dpc(
            kernel_arg(1u), kernel_arg(2u), kernel_arg(3u)));
}

static void bridge_ke_remove_queue_dpc(void)
{
    kernel_return(1u, recomp_kernel_remove_dpc(kernel_arg(1u)));
}

/* fastcall: the new IRQL arrives in ECX, not on the stack. */
static void bridge_kf_raise_irql(void)
{
    uint32_t previous = current_irql;

    current_irql = recomp_runtime.registers.ecx & 0xffu;
    kernel_return_caller_cleanup(previous);
}

static void bridge_kf_lower_irql(void)
{
    current_irql = recomp_runtime.registers.ecx & 0xffu;
    kernel_return_caller_cleanup(0u);
    if (current_irql < DISPATCH_LEVEL) {
        recomp_kernel_drain_dpcs();
    }
}

static void bridge_ke_raise_irql_to_dpc_level(void)
{
    uint32_t previous = current_irql;

    current_irql = DISPATCH_LEVEL;
    kernel_return(0u, previous);
}

static void bridge_ke_initialize_interrupt(void)
{
    uint32_t object = kernel_arg(1u);
    uint32_t routine = kernel_arg(2u);
    uint32_t context = kernel_arg(3u);
    uint32_t vector = kernel_arg(4u);

    if (interrupt_count < sizeof interrupts / sizeof interrupts[0]) {
        interrupts[interrupt_count].object = object;
        interrupts[interrupt_count].service_routine = routine;
        interrupts[interrupt_count].service_context = context;
        interrupts[interrupt_count].vector = vector;
        ++interrupt_count;
    }
    kernel_return(7u, 0u);
}

static void bridge_ke_connect_interrupt(void)
{
    uint32_t object = kernel_arg(1u);
    uint32_t connected = 0u;

    for (unsigned i = 0u; i < interrupt_count; ++i) {
        if (interrupts[i].object == object) {
            connected = 1u;
            break;
        }
    }
    /* Nothing raises this interrupt yet. Report success so startup proceeds;
       the device that would fire it is not modelled. */
    kernel_return(1u, connected);
}

static void bridge_ke_synchronize_execution(void)
{
    uint32_t routine = kernel_arg(2u);
    uint32_t context = kernel_arg(3u);
    uint32_t result = 0u;

    if (routine != 0u) {
        uint32_t arguments[1];

        arguments[0] = context;
        kernel_call_guest(routine, arguments, 1u);
        result = recomp_runtime.registers.eax;
    }
    kernel_return(3u, result);
}

static void bridge_hal_get_interrupt_vector(void)
{
    uint32_t level = kernel_arg(1u);
    uint32_t irql_out = kernel_arg(2u);

    if (irql_out != 0u) {
        *recomp_memory_u32(irql_out) = 0u;
    }
    kernel_return(2u, 0x20u + level);
}

static void bridge_hal_register_shutdown_notification(void)
{
    uint32_t notification = kernel_arg(1u);
    uint32_t priority = kernel_arg(2u);

    fprintf(
        stderr,
        "recomp kernel: HalRegisterShutdownNotification notification=0x%08"
        PRIx32 " priority=0x%08" PRIx32 " policy=no-op\n",
        notification,
        priority);
    kernel_return(2u, 0u);
}

RecompFunction recomp_kernel_interrupt(uint32_t ordinal)
{
    switch (ordinal) {
    case 44u: return bridge_hal_get_interrupt_vector;
    case 47u: return bridge_hal_register_shutdown_notification;
    case 98u: return bridge_ke_connect_interrupt;
    case 107u: return bridge_ke_initialize_dpc;
    case 109u: return bridge_ke_initialize_interrupt;
    case 119u: return bridge_ke_insert_queue_dpc;
    case 129u: return bridge_ke_raise_irql_to_dpc_level;
    case 137u: return bridge_ke_remove_queue_dpc;
    case 153u: return bridge_ke_synchronize_execution;
    case 160u: return bridge_kf_raise_irql;
    case 161u: return bridge_kf_lower_irql;
    default: return NULL;
    }
}
/* Rollback snapshots: queued DPCs, IRQL and connected interrupts. */
typedef struct InterruptState {
    QueuedDpc dpc_queue[QUEUE_CAPACITY];
    unsigned dpc_queue_count;
    uint32_t current_irql;
    InterruptRecord interrupts[32];
    unsigned interrupt_count;
} InterruptState;

size_t recomp_kernel_interrupt_state_size(void) { return sizeof(InterruptState); }

void recomp_kernel_interrupt_state_save(void *state)
{
    InterruptState *out = state;
    memcpy(out->dpc_queue, dpc_queue, sizeof dpc_queue);
    out->dpc_queue_count = dpc_queue_count;
    out->current_irql = current_irql;
    memcpy(out->interrupts, interrupts, sizeof interrupts);
    out->interrupt_count = interrupt_count;
}

void recomp_kernel_interrupt_state_restore(const void *state)
{
    const InterruptState *in = state;
    memcpy(dpc_queue, in->dpc_queue, sizeof dpc_queue);
    dpc_queue_count = in->dpc_queue_count;
    current_irql = in->current_irql;
    memcpy(interrupts, in->interrupts, sizeof interrupts);
    interrupt_count = in->interrupt_count;
}
