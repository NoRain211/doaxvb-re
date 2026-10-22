#include "cri_service_adapter.h"

#include "program_manual.h"

#include <stdio.h>
#include <string.h>

enum {
    TEST_STATIC_BASE = 0x003b7000u,
    TEST_STATIC_SIZE = 0x00004000u,
    TEST_STACK_BASE = 0x25000000u,
    TEST_STACK_SIZE = 0x00001000u,
    TEST_ENTRY_ESP = TEST_STACK_BASE + 0x100u,
    TEST_SYNC_FLAG = 0x003b772cu,
};

static uint32_t call_order;
static uint32_t lane2_entry_esp;
static uint32_t file_worker_io_entry_esp;
static uint32_t file_worker_entry_esp;
static uint32_t lane5_entry_esp;
static uint32_t stat_entry_esp;
static uint32_t parser_entry_esp;
static uint32_t adxf_open_entry_esp;
static uint32_t lane5_flag;

static int expect_u32(const char *field, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(
        stderr,
        "CRI service adapter: %s was 0x%08x, expected 0x%08x\n",
        field,
        actual,
        expected);
    return 0;
}

static void lane2_service(void)
{
    call_order = call_order * 10u + 2u;
    lane2_entry_esp = recomp_runtime.registers.esp;
    recomp_runtime.registers.eax = 0x22222222u;
    recomp_runtime.registers.ecx = 0xccccccccu;
    recomp_runtime.registers.esp += 4u;
}

static void lane5_service(void)
{
    call_order = call_order * 10u + 5u;
    lane5_entry_esp = recomp_runtime.registers.esp;
    lane5_flag = *recomp_memory_u32(TEST_SYNC_FLAG);
    recomp_runtime.registers.eax = 0x55555555u;
    recomp_runtime.registers.esp += 4u;
}

static void file_worker_service(void)
{
    call_order = call_order * 10u + 9u;
    file_worker_entry_esp = recomp_runtime.registers.esp;
    recomp_runtime.registers.eax = 0x99999999u;
    recomp_runtime.registers.esp += 4u;
}

static void file_worker_io(void)
{
    call_order = call_order * 10u + 4u;
    file_worker_io_entry_esp = recomp_runtime.registers.esp;
    recomp_runtime.registers.eax = 0x44444444u;
    recomp_runtime.registers.esp += 4u;
}

static void adxf_get_pt_stat(void)
{
    call_order = call_order * 10u + 7u;
    parser_entry_esp = recomp_runtime.registers.esp;
    recomp_runtime.registers.eax = 3u;
    recomp_runtime.registers.esp += 4u;
}

static void adxf_get_stat(void)
{
    call_order = call_order * 10u + 8u;
    stat_entry_esp = recomp_runtime.registers.esp;
    recomp_runtime.registers.eax = 4u;
    recomp_runtime.registers.esp += 4u;
}

static void adxf_open(void)
{
    call_order = call_order * 10u + 1u;
    adxf_open_entry_esp = recomp_runtime.registers.esp;
    recomp_runtime.registers.eax = 0x00ee1540u;
    recomp_runtime.registers.esp += 4u;
}

static int expect_lookup(uint32_t address)
{
    RecompFunction adapter = recomp_cri_service_lookup_manual(address);

    if (adapter != NULL && recomp_lookup_manual(address) == adapter) {
        return 1;
    }
    fprintf(
        stderr,
        "CRI service adapter: lookup failed for 0x%08x\n",
        address);
    return 0;
}

int recomp_cri_service_adapter_test(void)
{
    static uint8_t static_memory[TEST_STATIC_SIZE];
    static uint8_t stack_memory[TEST_STACK_SIZE];
    const RecompMemoryRegion regions[] = {
        {
            .address = TEST_STATIC_BASE,
            .size = sizeof static_memory,
            .data = static_memory,
        },
        {
            .address = TEST_STACK_BASE,
            .size = sizeof stack_memory,
            .data = stack_memory,
        },
    };
    const RecompCriServiceHooks hooks = {
        .lane2_service = lane2_service,
        .file_worker_io = file_worker_io,
        .file_worker_service = file_worker_service,
        .lane5_service = lane5_service,
        .adxf_get_stat = adxf_get_stat,
        .adxf_get_pt_stat = adxf_get_pt_stat,
        .adxf_open = adxf_open,
        .adxt_get_stat = adxf_get_stat,
    };
    const RecompCriServiceModel *model;
    RecompFunction adapter;
    int passed = 1;

    memset(static_memory, 0, sizeof static_memory);
    memset(stack_memory, 0, sizeof stack_memory);
    recomp_runtime_init(regions, 2u, NULL, 0u, NULL, 0u);
    recomp_cri_service_adapter_reset();
    recomp_cri_service_adapter_set_hooks(&hooks);
    model = recomp_cri_service_adapter_model();

    passed &= expect_lookup(0x001877d0u);
    passed &= expect_lookup(0x00187b30u);
    passed &= expect_lookup(0x00187e40u);
    passed &= expect_lookup(0x00189cb0u);
    passed &= expect_lookup(0x00188590u);
    if (recomp_cri_service_lookup_manual(0x001877cfu) != NULL ||
        recomp_cri_service_lookup_manual(0x001877d1u) != NULL ||
        recomp_cri_service_lookup_manual(0x00187b2fu) != NULL ||
        recomp_cri_service_lookup_manual(0x00187b31u) != NULL ||
        recomp_cri_service_lookup_manual(0x00187e3fu) != NULL ||
        recomp_cri_service_lookup_manual(0x00187e41u) != NULL ||
        recomp_cri_service_lookup_manual(0x00189cafu) != NULL ||
        recomp_cri_service_lookup_manual(0x00189cb1u) != NULL ||
        recomp_cri_service_lookup_manual(0x0018858fu) != NULL ||
        recomp_cri_service_lookup_manual(0x00188591u) != NULL) {
        fprintf(stderr, "CRI service adapter: lookup was not exact\n");
        return 0;
    }

    call_order = 0u;
    recomp_runtime.registers = (RecompRegisters){
        .eax = 0x11111111u,
        .ecx = 0x33333333u,
        .esp = TEST_ENTRY_ESP,
    };
    adapter = recomp_cri_service_lookup_manual(0x00187b30u);
    adapter();
    passed &= expect_u32("status order", call_order, 297u);
    passed &= expect_u32(
        "lane 2 entry ESP", lane2_entry_esp, TEST_ENTRY_ESP - 4u);
    passed &= expect_u32(
        "file worker entry ESP", file_worker_entry_esp,
        TEST_ENTRY_ESP - 4u);
    passed &= expect_u32(
        "parser entry ESP", parser_entry_esp, TEST_ENTRY_ESP);
    passed &= expect_u32(
        "status return ESP", recomp_runtime.registers.esp,
        TEST_ENTRY_ESP + 4u);
    passed &= expect_u32("status return EAX", recomp_runtime.registers.eax, 3u);
    passed &= expect_u32(
        "service register isolation", recomp_runtime.registers.ecx,
        0x33333333u);
    passed &= expect_u32("lane 2 batches", model->lane2_batches, 1u);
    passed &= expect_u32("status lane 5 batches", model->lane5_handoffs, 0u);

    call_order = 0u;
    recomp_runtime.registers = (RecompRegisters){
        .eax = 0x11111111u,
        .ecx = 0x33333333u,
        .esp = TEST_ENTRY_ESP,
    };
    adapter = recomp_cri_service_lookup_manual(0x001877d0u);
    adapter();
    /* ADXF_GetStat must drive the full file-worker step (lane 2, worker I/O,
       worker service) before the stat read, or a resource load queued while
       the guest spins on this poll can never complete. */
    passed &= expect_u32("stream status order", call_order, 2498u);
    passed &= expect_u32(
        "stream status worker I/O entry ESP",
        file_worker_io_entry_esp,
        TEST_ENTRY_ESP - 4u);
    passed &= expect_u32("stream status entry ESP", stat_entry_esp, TEST_ENTRY_ESP);
    passed &= expect_u32(
        "stream status return ESP", recomp_runtime.registers.esp,
        TEST_ENTRY_ESP + 4u);
    passed &= expect_u32("stream status return EAX", recomp_runtime.registers.eax, 4u);
    passed &= expect_u32("stream status batches", model->lane2_batches, 2u);
    passed &= expect_u32(
        "stream status lane 5 batches", model->lane5_handoffs, 0u);

    call_order = 0u;
    *recomp_memory_u32(TEST_ENTRY_ESP) = 0x0010abcdu;
    *recomp_memory_u32(TEST_ENTRY_ESP + 4u) = 0u;
    *recomp_memory_u32(TEST_ENTRY_ESP + 8u) = 0x00c2u;
    recomp_runtime.registers = (RecompRegisters){
        .eax = 0x11111111u,
        .ecx = 0x33333333u,
        .esp = TEST_ENTRY_ESP,
    };
    adapter = recomp_cri_service_lookup_manual(0x00187e40u);
    adapter();
    passed &= expect_u32("ADXF open order", call_order, 2491u);
    passed &= expect_u32(
        "ADXF open entry ESP", adxf_open_entry_esp, TEST_ENTRY_ESP);
    passed &= expect_u32(
        "ADXF open archive argument",
        *recomp_memory_u32(TEST_ENTRY_ESP + 4u),
        0u);
    passed &= expect_u32(
        "ADXF open member argument",
        *recomp_memory_u32(TEST_ENTRY_ESP + 8u),
        0x00c2u);
    passed &= expect_u32(
        "ADXF open return ESP",
        recomp_runtime.registers.esp,
        TEST_ENTRY_ESP + 4u);
    passed &= expect_u32(
        "ADXF open return EAX", recomp_runtime.registers.eax, 0x00ee1540u);
    passed &= expect_u32(
        "ADXF open register isolation",
        recomp_runtime.registers.ecx,
        0x33333333u);
    passed &= expect_u32("ADXF open batches", model->lane2_batches, 3u);
    passed &= expect_u32(
        "ADXF open lane 5 batches", model->lane5_handoffs, 0u);

    call_order = 0u;
    *recomp_memory_u32(TEST_SYNC_FLAG) = 0u;
    recomp_runtime.registers = (RecompRegisters){
        .eax = 0x11111111u,
        .esp = TEST_ENTRY_ESP,
    };
    adapter = recomp_cri_service_lookup_manual(0x00189cb0u);
    adapter();
    passed &= expect_u32("sync order", call_order, 5u);
    passed &= expect_u32(
        "lane 5 entry ESP", lane5_entry_esp, TEST_ENTRY_ESP - 4u);
    passed &= expect_u32("lane 5 observed flag", lane5_flag, 1u);
    passed &= expect_u32(
        "lane 5 cleared flag", *recomp_memory_u32(TEST_SYNC_FLAG), 0u);
    passed &= expect_u32(
        "sync return ESP", recomp_runtime.registers.esp,
        TEST_ENTRY_ESP + 4u);
    passed &= expect_u32("sync return EAX", recomp_runtime.registers.eax, 1u);
    passed &= expect_u32("lane 5 handoffs", model->lane5_handoffs, 1u);

    call_order = 0u;
    recomp_runtime.registers = (RecompRegisters){
        .eax = 0x11111111u,
        .ecx = 0x33333333u,
        .esp = TEST_ENTRY_ESP,
    };
    adapter = recomp_cri_service_lookup_manual(0x00188590u);
    adapter();
    passed &= expect_u32("ADXT status worker order", call_order, 2498u);
    passed &= expect_u32("ADXT status entry ESP", stat_entry_esp, TEST_ENTRY_ESP);
    passed &= expect_u32("ADXT status return ESP",
        recomp_runtime.registers.esp, TEST_ENTRY_ESP + 4u);
    passed &= expect_u32("ADXT status return EAX", recomp_runtime.registers.eax, 4u);
    passed &= expect_u32("ADXT service register isolation",
        recomp_runtime.registers.ecx, 0x33333333u);
    passed &= expect_u32("ADXT worker batches", model->lane2_batches, 4u);
    recomp_cri_service_adapter_reset();
    return passed;
}
