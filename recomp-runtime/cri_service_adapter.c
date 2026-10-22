#include "cri_service_adapter.h"

#include "stop_report.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum {
    ADXF_GET_STAT_ADDRESS = 0x001877d0u,
    ADXT_GET_STAT_ADDRESS = 0x00188590u,
    ADXF_OPEN_ADDRESS = 0x00187e40u,
    ADXF_GET_PT_STAT_ADDRESS = 0x00187b30u,
    CRI_SYNC_CALLBACK_ADDRESS = 0x00189cb0u,
    CRI_LANE2_SERVICE_ADDRESS = 0x00193e30u,
    CRI_LANE5_SERVICE_ADDRESS = 0x00193e50u,
    CRI_SYNC_FLAG_ADDRESS = 0x003b772cu,
    CRI_PARTITION_COUNT_ADDRESS = 0x005e69d0u,
};

#ifdef RECOMP_FULL_PROGRAM
void sub_001877D0(void);
void sub_00187B30(void);
void sub_00187E40(void);
void sub_00188590(void);
void sub_00189970(void);
void sub_00193E30(void);
void sub_00193E50(void);
void sub_001E38F0(void);

static const RecompCriServiceHooks default_hooks = {
    .lane2_service = sub_00193E30,
    .file_worker_io = sub_001E38F0,
    .file_worker_service = sub_00189970,
    .lane5_service = sub_00193E50,
    .adxf_get_stat = sub_001877D0,
    .adxf_get_pt_stat = sub_00187B30,
    .adxf_open = sub_00187E40,
    .adxt_get_stat = sub_00188590,
};
#else
static const RecompCriServiceHooks default_hooks;
#endif

typedef struct RecompCriGuestStep {
    const RecompFunction *functions;
    size_t function_count;
    int balanced;
} RecompCriGuestStep;

static RecompCriServiceModel service_model;
static RecompCriServiceHooks service_hooks;
static int reported_lane2;
static int reported_lane5;
static int reported_catalog;
static int reported_adxf_open;
static uint32_t adxf_get_stat_calls;

uint32_t recomp_cri_service_adxf_get_stat_calls(void)
{
    return adxf_get_stat_calls;
}

static uint32_t file_worker_steps;

uint32_t recomp_cri_service_file_worker_steps(void)
{
    return file_worker_steps;
}

static void run_guest_step(void *context)
{
    RecompCriGuestStep *step = context;
    step->balanced = 1;
    for (size_t i = 0u; i < step->function_count; ++i) {
        RecompRegisters saved = recomp_runtime.registers;

        recomp_runtime.registers.esp -= 4u;
        *recomp_memory_u32(recomp_runtime.registers.esp) = 0u;
        step->functions[i]();
        if (recomp_runtime.registers.esp != saved.esp) {
            step->balanced = 0;
        }
        recomp_runtime.registers = saved;
        if (!step->balanced) {
            break;
        }
    }
}

static RecompCriServiceResult run_service_batch(
    int lane,
    const RecompFunction *functions,
    size_t function_count)
{
    RecompCriGuestStep step = {
        .functions = functions,
        .function_count = function_count,
        .balanced = 0,
    };
    RecompCriServiceResult result;

    if (functions == NULL || function_count == 0u) {
        return RECOMP_CRI_SERVICE_INVALID;
    }
    for (size_t i = 0u; i < function_count; ++i) {
        if (functions[i] == NULL) {
            return RECOMP_CRI_SERVICE_INVALID;
        }
    }
    result = lane == 2
        ? recomp_cri_service_run_lane2(&service_model, run_guest_step, &step)
        : recomp_cri_service_run_lane5(&service_model, run_guest_step, &step);
    if (result == RECOMP_CRI_SERVICE_OK && !step.balanced) {
        recomp_stop(2, "cri-service:lane-%d-stack", lane);
    }
    return result;
}

static RecompCriServiceResult run_lane2_batch(void)
{
    const RecompFunction functions[] = {
        service_hooks.lane2_service,
        service_hooks.file_worker_service,
    };

    return run_service_batch(
        2, functions, sizeof functions / sizeof functions[0]);
}

static RecompCriServiceResult run_file_worker_step(void)
{
    const RecompFunction functions[] = {
        service_hooks.lane2_service,
        service_hooks.file_worker_io,
        service_hooks.file_worker_service,
    };

    ++file_worker_steps;
    return run_service_batch(
        2, functions, sizeof functions / sizeof functions[0]);
}

static void require_lane2_service(RecompCriServiceResult result)
{
    if (result == RECOMP_CRI_SERVICE_INVALID) {
        recomp_stop(2, "cri-service:lane-2-unavailable");
    }
    if (result == RECOMP_CRI_SERVICE_OK && !reported_lane2) {
        fprintf(
            stderr,
            "recomp cri: cooperative lane-2 batch count=%" PRIu32 "\n",
            service_model.lane2_batches);
        reported_lane2 = 1;
    }
}

static void recomp_adxf_get_stat_adapter(void)
{
    /* The XPR resource wait at 0x000F3A40 spins on ADXF_GetStat and does
       nothing else, so this poll is the only chance the file worker gets to
       advance a queued read. Pumping the lane-2 batch alone runs the worker's
       bookkeeping without ever issuing the transfer at 0x001E38F0, so a load
       queued during the spin never completes. Drive the same full worker step
       ADXF_OpenAfs and MWP_FrameGetStatus already use. */
    RecompCriServiceResult lane2_result = run_file_worker_step();

    if (service_hooks.adxf_get_stat == NULL) {
        recomp_stop(2, "cri-service:lane-2-unavailable");
    }
    require_lane2_service(lane2_result);
    service_hooks.adxf_get_stat();
    ++adxf_get_stat_calls;
#ifdef RECOMP_FULL_PROGRAM
    /* 0x000A32F0 only retires a request when the stat reaches 3 or 4, so a
       stat pinned below that leaves the queue head polled forever. Report
       each distinct value once to show which state it settles in. */
    {
        static uint32_t seen_mask;
        uint32_t status = recomp_runtime.registers.eax;
        uint32_t bit = status < 32u ? (1u << status) : 0x80000000u;

        if ((seen_mask & bit) == 0u) {
            seen_mask |= bit;
            fprintf(
                stderr,
                "recomp cri: ADXF_GetStat status=%" PRIu32
                " call=%" PRIu32 "\n",
               status, adxf_get_stat_calls);
       }
   }
    /* Every CRI guard copies its message into the fixed buffer at 0x00EDFFC0
       before it returns, so the last string names the exact check that
       refused a read. ADXF_ReadNw rejects a buffer that is not 4-byte
       aligned without ever reaching ReadNw32, which leaves the handle open
       at state 1 with no stream joint - the observed stall. */
    {
        static char seen[64];
        static uint32_t err_prints;
        const char *err = (const char *)recomp_memory_i8(0x00edffc0u);

        if (err[0] != '\0' && err_prints < 8u
            && memcmp(seen, err, sizeof seen) != 0) {
            memcpy(seen, err, sizeof seen);
            ++err_prints;
            fprintf(
                stderr,
                "recomp cri: guard \"%.120s\" call=%" PRIu32 "\n",
                err, adxf_get_stat_calls);
        }
    }
    /* The ADXF stream mirrors the lower file object's status byte, so a
       stat pinned at 1 (READING) means that object stopped advancing. Dump
       both blocks once per distinct stream state to show which layer is
       stuck and whether a transfer is still outstanding. */
    {
        static uint32_t last_key = 0xffffffffu;
        static uint32_t prints;
        /* 0x000A32F0 polls whichever handle the channel row holds, not the
           first table slot, so read the live handle from 0x005DEC68 and fall
           back to slot 0 only before a channel is open. */
        uint32_t stream = *recomp_memory_u32(0x005dec68u);
        uint32_t used;
        uint32_t state;
        uint32_t busy;
        uint32_t retry;
        uint32_t file;
        uint32_t xfer;
        uint32_t key;

        if (stream < 0x00ee1540u || stream >= 0x00ee28c0u) {
            stream = 0x00ee1540u;
        }
        used = (uint32_t)(uint8_t)*recomp_memory_i8(stream);
        state = (uint32_t)(uint8_t)*recomp_memory_i8(stream + 1u);
        busy = (uint32_t)(uint8_t)*recomp_memory_i8(stream + 2u);
        retry = (uint32_t)(uint8_t)*recomp_memory_i8(stream + 3u);
        file = *recomp_memory_u32(stream + 4u);
        xfer = *recomp_memory_u32(stream + 8u);
        key = (used << 24) | (state << 16) | (busy << 8) | retry;

        if (key != last_key && adxf_get_stat_calls > 90u && prints < 12u) {
            last_key = key;
            ++prints;
            fprintf(
                stderr,
                "recomp cri: adxf stream used=%" PRIu32 " state=%" PRIu32
                " busy=%" PRIu32 " retry=%" PRIu32 " file=0x%08" PRIx32
                " xfer=0x%08" PRIx32 " want=%" PRId32 " got=%" PRId32
                " pos=%" PRId32 " call=%" PRIu32 "\n",
                used, state, busy, retry, file, xfer,
                (int32_t)*recomp_memory_u32(stream + 0x1cu),
                (int32_t)*recomp_memory_u32(stream + 0x20u),
                (int32_t)*recomp_memory_u32(stream + 0x14u),
                adxf_get_stat_calls);
            /* ADXF_ReadNw32 stamps the caller's buffer at +0x24 and its
               length at +0x28 before it starts the stream, so a zero pair
               proves no read was ever requested on this handle rather than
               one being requested and stalling. The channel row at
               0x005DEC58 holds the handle the XPR poller actually watches,
               so print it too to catch the handle being recycled. */
            fprintf(
                stderr,
                "recomp cri: adxf hnd=0x%08" PRIx32 " fsizesct=%" PRId32
                " buf=0x%08" PRIx32 " nbytes=%" PRId32
                " row0=0x%08" PRIx32 " rowbuf=0x%08" PRIx32
                " rowsize=%" PRId32 "\n",
                stream,
                (int32_t)*recomp_memory_u32(stream + 0x0cu),
                *recomp_memory_u32(stream + 0x24u),
                (int32_t)*recomp_memory_u32(stream + 0x28u),
                *recomp_memory_u32(0x005dec68u),
                *recomp_memory_u32(0x005dec5cu),
                (int32_t)*recomp_memory_u32(0x005dec60u));
            /* 0x000F39E0 computes the load destination as [0x002A7AA4]
               masked to zero unless [0x009D9E1C] is greater than 1, so a
               NULL row buffer is that counter still sitting at its initial
               value rather than a lost pointer. Print both. */
            fprintf(
                stderr,
                "recomp cri: xpr gate count=%" PRId32
                " pool=0x%08" PRIx32 " mailbox=%" PRIu32
                " mbbuf=0x%08" PRIx32 "\n",
                (int32_t)*recomp_memory_u32(0x009d9e1cu),
                *recomp_memory_u32(0x002a7aa4u),
                (uint32_t)(uint8_t)*recomp_memory_i8(0x005dde79u),
                *recomp_memory_u32(0x005dde7cu));
            if (file != 0u) {
                fprintf(
                    stderr,
                    "recomp cri: adxf file state=%" PRIu32 " busy=%" PRIu32
                    " retry=%" PRIu32 " obj=0x%08" PRIx32
                    " nowait=%" PRIu32 "\n",
                    (uint32_t)(uint8_t)*recomp_memory_i8(file + 1u),
                    (uint32_t)(uint8_t)*recomp_memory_i8(file + 2u),
                    (uint32_t)(uint8_t)*recomp_memory_i8(file + 3u),
                    *recomp_memory_u32(file + 8u),
                    (uint32_t)(uint8_t)*recomp_memory_i8(file + 0x44u));
                fprintf(
                    stderr,
                    "recomp cri: adxf file used=%" PRIu32
                    " want=%" PRId32 " got=%" PRId32 " pos=%" PRId32
                    " limit=%" PRId32 " done=%" PRId32 "\n",
                    (uint32_t)(uint8_t)*recomp_memory_i8(file),
                    (int32_t)*recomp_memory_u32(file + 0x1cu),
                    (int32_t)*recomp_memory_u32(file + 0x20u),
                    (int32_t)*recomp_memory_u32(file + 0x14u),
                    (int32_t)*recomp_memory_u32(file + 0x30u),
                    (int32_t)*recomp_memory_u32(file + 0x34u));
                /* 0x0018AFE0 refuses to issue a transfer when the device at
                   +4 has no vtable (counted in 0x003B77A4) and when the
                   sector budget at +0x1C is zero, so print both guards. */
                {
                    uint32_t dev = *recomp_memory_u32(file + 4u);
                    uint32_t vtbl = dev != 0u
                        ? *recomp_memory_u32(dev)
                        : 0u;

                    fprintf(
                        stderr,
                        "recomp cri: adxf dev=0x%08" PRIx32
                        " vtbl=0x%08" PRIx32 " reqrd=0x%08" PRIx32
                        " nodev=%" PRIu32 " retrylimit=%" PRId32
                        " sectors=%" PRId32 " size=%" PRId32 "\n",
                        dev, vtbl,
                        vtbl != 0u ? *recomp_memory_u32(vtbl + 0x20u) : 0u,
                        *recomp_memory_u32(0x003b77a4u),
                        (int32_t)*recomp_memory_u32(0x003b77a0u),
                        (int32_t)*recomp_memory_u32(file + 0x10u),
                        (int32_t)*recomp_memory_u32(file + 0x0cu));
                }
            }
            /* The ADX reader objects at 0x00ed96c0 own the actual transfer:
               +0x44 asks the pump to issue a read and +0x48 marks one in
               flight. Whichever flag is stuck names the half of the pump
               that stopped running. */
            {
                uint32_t entry;
                for (entry = 0x00ed96c0u; entry < 0x00edcbe0u;
                     entry += 0x154u) {
                    uint32_t used =
                        (uint32_t)(uint8_t)*recomp_memory_i8(entry);
                    uint32_t rstate =
                        (uint32_t)(uint8_t)*recomp_memory_i8(entry + 1u);

                    if (used == 0u && rstate == 0u) {
                        continue;
                    }
                    fprintf(
                        stderr,
                        "recomp cri: adx rdr 0x%08" PRIx32 " used=%" PRIu32
                        " state=%" PRIu32 " issue=%" PRId32
                        " inflight=%" PRId32 " handle=0x%08" PRIx32
                        " ovl=0x%08" PRIx32 " got=%" PRId32 "\n",
                        entry, used, rstate,
                        (int32_t)*recomp_memory_u32(entry + 0x44u),
                        (int32_t)*recomp_memory_u32(entry + 0x48u),
                        *recomp_memory_u32(entry + 0x24u),
                        *recomp_memory_u32(entry + 0x28u),
                        (int32_t)*recomp_memory_u32(entry + 0x40u));
                }
            }
        }
    }
#endif
}

static void recomp_adxt_get_stat_adapter(void)
{
    /* ADXT's startup state advances in its registered lane-2 callback.
       Status-only voice waits must service that worker just like file waits. */
    RecompCriServiceResult result = run_file_worker_step();

    if (service_hooks.adxt_get_stat == NULL) {
        recomp_stop(2, "cri-service:adxt-status-unavailable");
    }
    require_lane2_service(result);
    service_hooks.adxt_get_stat();
}

static void recomp_adxf_get_pt_stat_adapter(void)
{
    RecompCriServiceResult lane2_result = run_lane2_batch();

    if (service_hooks.adxf_get_pt_stat == NULL) {
        recomp_stop(2, "cri-service:lane-2-unavailable");
    }
    require_lane2_service(lane2_result);
    service_hooks.adxf_get_pt_stat();
#ifdef RECOMP_FULL_PROGRAM
    if (!reported_catalog && recomp_runtime.registers.eax == 3u &&
        *recomp_memory_u32(CRI_PARTITION_COUNT_ADDRESS) == 0x101u) {
        fprintf(
            stderr,
            "recomp cri: generated ADXF_GetPtStat return=3 count=0x101\n");
        reported_catalog = 1;
        recomp_stop_at_boundary("cri-adxf-catalog:ready");
    }
#endif
}

static void recomp_adxf_open_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    RecompCriServiceResult lane2_result = run_file_worker_step();

    if (service_hooks.adxf_open == NULL) {
        recomp_stop(2, "cri-service:adxf-open-unavailable");
    }
    require_lane2_service(lane2_result);
    service_hooks.adxf_open();
    if (!reported_adxf_open && recomp_runtime.registers.eax != 0u) {
        fprintf(
            stderr,
            "recomp cri: generated ADXF_OpenAfs archive=%" PRIu32
            " member=%" PRIu32 " handle=0x%08" PRIx32 "\n",
            *recomp_memory_u32(entry_esp + 4u),
            *recomp_memory_u32(entry_esp + 8u),
            recomp_runtime.registers.eax);
        reported_adxf_open = 1;
        recomp_stop_at_boundary("cri-adxf-open:first-success");
    }
}

static void recomp_cri_sync_callback_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    RecompCriServiceResult result;

    *recomp_memory_u32(CRI_SYNC_FLAG_ADDRESS) = 1u;
    result = run_service_batch(5, &service_hooks.lane5_service, 1u);
    if (result != RECOMP_CRI_SERVICE_OK) {
        recomp_stop(2, "cri-service:lane-5-result:%u", (unsigned)result);
    }
    if (*recomp_memory_u32(CRI_SYNC_FLAG_ADDRESS) != 1u) {
        recomp_stop(2, "cri-service:lane-5-handshake");
    }
    *recomp_memory_u32(CRI_SYNC_FLAG_ADDRESS) = 0u;
    if (!reported_lane5) {
        fprintf(
            stderr,
            "recomp cri: cooperative lane-5 handoff count=%" PRIu32 "\n",
            service_model.lane5_handoffs);
        reported_lane5 = 1;
    }
    recomp_runtime.registers.eax = 1u;
    recomp_runtime.registers.esp = entry_esp + 4u;
}

void recomp_cri_service_adapter_reset(void)
{
    recomp_cri_service_reset(&service_model);
    service_hooks = default_hooks;
    reported_lane2 = 0;
    reported_lane5 = 0;
    reported_catalog = 0;
    reported_adxf_open = 0;
    adxf_get_stat_calls = 0u;
    file_worker_steps = 0u;
}

void recomp_cri_service_adapter_set_hooks(
    const RecompCriServiceHooks *hooks)
{
    service_hooks = hooks != NULL
        ? *hooks
        : (RecompCriServiceHooks){0};
}

const RecompCriServiceModel *recomp_cri_service_adapter_model(void)
{
    return &service_model;
}

RecompFunction recomp_cri_service_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case ADXF_GET_STAT_ADDRESS:
        return recomp_adxf_get_stat_adapter;
    case ADXT_GET_STAT_ADDRESS:
        return recomp_adxt_get_stat_adapter;
    case ADXF_GET_PT_STAT_ADDRESS:
        return recomp_adxf_get_pt_stat_adapter;
    case ADXF_OPEN_ADDRESS:
        return recomp_adxf_open_adapter;
    case CRI_SYNC_CALLBACK_ADDRESS:
        return recomp_cri_sync_callback_adapter;
    default:
        return NULL;
    }
}
