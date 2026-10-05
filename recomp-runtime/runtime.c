#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "runtime.h"
#include "apu_model.h"
#include "fiber_adapter.h"
#include "ohci_model.h"
#include "stop_report.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <execinfo.h>
#include <dlfcn.h>
#endif

RecompRuntime recomp_runtime;
uint32_t recomp_last_dispatch_address;
/* Generated code pushes a literal return slot, so the guest stack cannot be
   walked. Dispatch is the only place a caller is known, so record the frames
   there and the innermost one names the function that faulted. */
typedef struct RecompDispatchFrame {
    uint32_t guest_address;
    const char *member;
    int line;
} RecompDispatchFrame;

static RecompDispatchFrame dispatch_stack[64];
static size_t dispatch_depth;
static uint8_t *direct_ram;
uint8_t *recomp_fast_ram;

static void report_dispatch_stack(void);
static void watch_init(void);
static void update_fast_ram(void);

enum {
    XBOX_RAM_SIZE = RECOMP_XBOX_RAM_SIZE,
    XBOX_CACHED_ALIAS = RECOMP_XBOX_CACHED_ALIAS,
    XBOX_PHYSICAL_ALIAS = RECOMP_XBOX_PHYSICAL_ALIAS,
    USB0_OHCI_BASE = 0xfed00000u,
};

typedef struct RecompMmioU32Access {
    bool pending;
    uint32_t guest_address;
    RecompOhciRegister reg;
    uint32_t original;
    uint32_t value;
} RecompMmioU32Access;

static RecompOhci usb0_ohci;
static RecompApu mcpx_apu;
static RecompMmioU32Access mmio_u32_access;

void recomp_runtime_init(
    const RecompMemoryRegion *memory_regions,
    size_t memory_region_count,
    RecompMemoryAccess *accesses,
    size_t access_capacity,
    const RecompFunctionEntry *functions,
    size_t function_count)
{
    recomp_runtime = (RecompRuntime){
        .memory_regions = memory_regions,
        .memory_region_count = memory_region_count,
        .accesses = accesses,
        .access_capacity = access_capacity,
        .functions = functions,
        .function_count = function_count,
    };
    direct_ram = memory_region_count == 1u && memory_regions != NULL &&
        memory_regions[0].address == 0u &&
        memory_regions[0].size == 0x04000000u
        ? memory_regions[0].data : NULL;
    /* x87 reset default: all exceptions masked, round-to-nearest,
       extended precision. The guest CRT reads this back through FNSTCW. */
    recomp_runtime.fpu_control_word = 0x037fu;
    recomp_runtime.fpu_compare = 0;
    recomp_ohci_init(&usb0_ohci);
    recomp_apu_init(&mcpx_apu);
    mmio_u32_access = (RecompMmioU32Access){0};
    watch_init();
    update_fast_ram();
}

void recomp_runtime_set_lookup(RecompFunctionLookup lookup)
{
    recomp_runtime.lookup = lookup;
}

static void fail_memory_access(uint32_t guest_address, size_t width)
{
    ++recomp_runtime.undeclared_access_count;
    fprintf(
        stderr,
        "recomp runtime: undeclared %zu-byte guest memory access at 0x%08"
        PRIx32 "\n",
        width,
        guest_address);
    /* The faulting address is usually a base register plus a constant, so the
       registers say which pointer went bad and the last dispatch says where. */
    fprintf(
        stderr,
        "recomp runtime: undeclared access registers eax=0x%08" PRIx32
        " ecx=0x%08" PRIx32 " edx=0x%08" PRIx32 " ebx=0x%08" PRIx32
        " esi=0x%08" PRIx32 " edi=0x%08" PRIx32 " ebp=0x%08" PRIx32
        " esp=0x%08" PRIx32 " last-dispatch=0x%08" PRIx32 "\n",
        recomp_runtime.registers.eax,
        recomp_runtime.registers.ecx,
        recomp_runtime.registers.edx,
        recomp_runtime.registers.ebx,
        recomp_runtime.registers.esi,
        recomp_runtime.registers.edi,
        recomp_runtime.registers.ebp,
        recomp_runtime.registers.esp,
        recomp_last_dispatch_address);
    report_dispatch_stack();
#ifdef _WIN32
    if (getenv("RECOMP_FAULT_TRACE") != NULL) {
        void *frames[32];
        USHORT count = CaptureStackBackTrace(0u, 32u, frames, NULL);
        fprintf(stderr, "[DEBUG-r501-fault] module=%p frames=%u\n",
            (void *)GetModuleHandleW(NULL), (unsigned)count);
        for (USHORT i = 0u; i < count; ++i) {
            fprintf(stderr, "[DEBUG-r501-fault] frame=%u pc=%p\n",
                (unsigned)i, frames[i]);
        }
    }
#else
    if (getenv("RECOMP_FAULT_TRACE") != NULL) {
        void *frames[32];
        int count = backtrace(frames, 32);
        for (int i = 0; i < count; ++i) {
            Dl_info symbol = {0};
            dladdr(frames[i], &symbol);
            fprintf(stderr, "[DEBUG-r501-fault] frame=%u pc=%p symbol=%s module=%s\n",
                (unsigned)i, frames[i],
                symbol.dli_sname != NULL ? symbol.dli_sname : "<unresolved>",
                symbol.dli_fname != NULL ? symbol.dli_fname : "<unresolved>");
        }
    }
#endif
    recomp_stop(2, "memory:0x%08" PRIx32, guest_address);
}

static void record_memory_access(uint32_t guest_address, size_t width)
{
    if (recomp_runtime.accesses == NULL) {
        return;
    }
    if (recomp_runtime.access_count >= recomp_runtime.access_capacity) {
        fprintf(stderr, "recomp runtime: memory access log full\n");
        recomp_stop(2, "access-log-full");
    }

    recomp_runtime.accesses[recomp_runtime.access_count++] =
        (RecompMemoryAccess){
            .address = guest_address,
            .width = (uint32_t)width,
        };
}

/* MEM32 is an lvalue. Commit a changed staging word on the next memory access
   so device write semantics stay in the model rather than in interception. */
static void commit_mmio_u32_write(void)
{
    if (!mmio_u32_access.pending) {
        return;
    }

    mmio_u32_access.pending = false;
    update_fast_ram();
    if (mmio_u32_access.value == mmio_u32_access.original) {
        return;
    }
    if (!recomp_ohci_write(
            &usb0_ohci,
            mmio_u32_access.reg,
            mmio_u32_access.value)) {
        fprintf(
            stderr,
            "recomp runtime: unsupported OHCI write 0x%08" PRIx32
            " at 0x%08" PRIx32 "\n",
            mmio_u32_access.value,
            mmio_u32_access.guest_address);
        recomp_stop(
            2, "ohci-write:0x%08" PRIx32, mmio_u32_access.guest_address);
    }
}

static bool usb0_ohci_register(
    uint32_t guest_address,
    RecompOhciRegister *reg)
{
    switch (guest_address) {
    case USB0_OHCI_BASE + 0x00u:
        *reg = RECOMP_OHCI_HC_REVISION;
        return true;
    case USB0_OHCI_BASE + 0x04u:
        *reg = RECOMP_OHCI_HC_CONTROL;
        return true;
    case USB0_OHCI_BASE + 0x08u:
        *reg = RECOMP_OHCI_HC_COMMAND_STATUS;
        return true;
    case USB0_OHCI_BASE + 0x10u:
        *reg = RECOMP_OHCI_HC_INTERRUPT_ENABLE;
        return true;
    case USB0_OHCI_BASE + 0x18u:
        *reg = RECOMP_OHCI_HC_HCCA;
        return true;
    case USB0_OHCI_BASE + 0x1cu:
        *reg = RECOMP_OHCI_HC_PERIOD_CURRENT_ED;
        return true;
    case USB0_OHCI_BASE + 0x20u:
        *reg = RECOMP_OHCI_HC_CONTROL_HEAD_ED;
        return true;
    case USB0_OHCI_BASE + 0x24u:
        *reg = RECOMP_OHCI_HC_CONTROL_CURRENT_ED;
        return true;
    case USB0_OHCI_BASE + 0x28u:
        *reg = RECOMP_OHCI_HC_BULK_HEAD_ED;
        return true;
    case USB0_OHCI_BASE + 0x2cu:
        *reg = RECOMP_OHCI_HC_BULK_CURRENT_ED;
        return true;
    case USB0_OHCI_BASE + 0x30u:
        *reg = RECOMP_OHCI_HC_DONE_HEAD;
        return true;
    case USB0_OHCI_BASE + 0x34u:
        *reg = RECOMP_OHCI_HC_FM_INTERVAL;
        return true;
    case USB0_OHCI_BASE + 0x40u:
        *reg = RECOMP_OHCI_HC_PERIODIC_START;
        return true;
    case USB0_OHCI_BASE + 0x44u:
        *reg = RECOMP_OHCI_HC_LS_THRESHOLD;
        return true;
    case USB0_OHCI_BASE + 0x48u:
        *reg = RECOMP_OHCI_HC_RH_DESCRIPTOR_A;
        return true;
    case USB0_OHCI_BASE + 0x4cu:
        *reg = RECOMP_OHCI_HC_RH_DESCRIPTOR_B;
        return true;
    case USB0_OHCI_BASE + 0x50u:
        *reg = RECOMP_OHCI_HC_RH_STATUS;
        return true;
    case USB0_OHCI_BASE + 0x54u:
        *reg = RECOMP_OHCI_HC_RH_PORT_STATUS_1;
        return true;
    case USB0_OHCI_BASE + 0x58u:
        *reg = RECOMP_OHCI_HC_RH_PORT_STATUS_2;
        return true;
    case USB0_OHCI_BASE + 0x5cu:
        *reg = RECOMP_OHCI_HC_RH_PORT_STATUS_3;
        return true;
    case USB0_OHCI_BASE + 0x60u:
        *reg = RECOMP_OHCI_HC_RH_PORT_STATUS_4;
        return true;
    default:
        return false;
    }
}

static uint8_t *find_memory_region(uint32_t guest_address, size_t width)
{
    uint64_t access_end = (uint64_t)guest_address + width;

    for (size_t i = 0; i < recomp_runtime.memory_region_count; ++i) {
        const RecompMemoryRegion *region = &recomp_runtime.memory_regions[i];
        uint64_t region_end = (uint64_t)region->address + region->size;

        if (region->data != NULL &&
            guest_address >= region->address &&
            access_end <= region_end) {
            return region->data + (guest_address - region->address);
        }
    }

    return NULL;
}

/* Round 37 write-watch. Every guest memory access in generated code funnels
   through recomp_memory(), so polling one watched word here catches its next
   writer without a hardware breakpoint or a host debugger. The report names
   the writer through the dispatch stack, which carries __FILE__/__LINE__
   because the lifted guest stack does not carry a usable return address.
   Env-gated: RECOMP_WATCH is a hex guest address, and the watch is inert
   when it is unset. */
static struct {
    bool active;
    bool armed;
    uint32_t address;
    uint32_t value;
    uint32_t hits;
    uint32_t hit_limit;
} watch;

static void update_fast_ram(void)
{
    recomp_fast_ram = !watch.active && recomp_runtime.accesses == NULL &&
        !mmio_u32_access.pending ? direct_ram : NULL;
}

static void watch_init(void)
{
    const char *setting = getenv("RECOMP_WATCH");
    const char *limit;

    watch.active = false;
    if (setting == NULL || setting[0] == '\0') {
        return;
    }
    watch.address = (uint32_t)strtoul(setting, NULL, 16);
    if (watch.address == 0u) {
        return;
    }
    limit = getenv("RECOMP_WATCH_HITS");
    watch.hit_limit = limit != NULL ? (uint32_t)strtoul(limit, NULL, 10) : 40u;
    watch.active = true;
    watch.armed = false;
    watch.hits = 0u;
    fprintf(
        stderr,
        "recomp watch: watching 0x%08" PRIx32 " limit=%" PRIu32 "\n",
        watch.address,
        watch.hit_limit);
}

/* Reads a guest byte for the watch report without going through
   recomp_memory(), which would recurse into the watch itself. */
static int watch_byte(uint32_t guest_address)
{
    const uint8_t *data = find_memory_region(guest_address, 1u);

    return data != NULL ? (int)*data : -1;
}

/* Same idea as watch_byte, for a guest float. Returns 0 when unmapped. */
static float watch_float(uint32_t guest_address)
{
    const uint8_t *data = find_memory_region(guest_address, 4u);
    float value = 0.0f;

    if (data != NULL) {
        memcpy(&value, data, sizeof value);
    }
    return value;
}
static void report_watch_writer(uint32_t previous, uint32_t current)
{
    float previous_float;
    float current_float;
    size_t depth;

    memcpy(&previous_float, &previous, sizeof previous_float);
    memcpy(&current_float, &current, sizeof current_float);
    fprintf(
        stderr,
        "recomp watch: hit %" PRIu32 " at 0x%08" PRIx32
        " 0x%08" PRIx32 " -> 0x%08" PRIx32 " (%.9g -> %.9g, delta %.9g)"
        " fiber=0x%08" PRIx32 " idx=%d idx2=%d last-dispatch=0x%08" PRIx32 "\n",
        watch.hits,
        watch.address,
        previous,
        current,
        (double)previous_float,
        (double)current_float,
        (double)(current_float - previous_float),
        recomp_fiber_adapter_model()->current_handle,
        watch_byte(0x0041EFE7u),
        watch_byte(0x004D56CDu),
        recomp_last_dispatch_address);
    /* Round 41: the camera updater sub_0002EF54 walks a five-entry list of
       int16 slot indices at 0x41EFD8 and integrates every record whose
       occupancy byte (slot*0xAE0 + 0x41B2D8) is set, while the publisher
       renders only the selected slot. Dumping the list beside the occupancy
       bytes shows directly whether two entries name the same record. */
    {
        int list[5];
        int live[5];
        size_t entry;

        for (entry = 0u; entry < 5u; ++entry) {
            int low = watch_byte((uint32_t)(0x0041EFD8u + entry * 2u));
            int high = watch_byte((uint32_t)(0x0041EFD8u + entry * 2u + 1u));

            list[entry] = (low < 0 || high < 0)
                ? -1
                : (int)(int16_t)((unsigned)low | ((unsigned)high << 8));
            live[entry] =
                watch_byte((uint32_t)(0x0041B2D8u + entry * 0x0AE0u));
        }
        fprintf(
            stderr,
            "recomp watch:   camlist %d %d %d %d %d live %d %d %d %d %d\n",
            list[0], list[1], list[2], list[3], list[4],
            live[0], live[1], live[2], live[3], live[4]);
    }
    /* Round 45: the integrator sub_0002DFE0 switches on the mode byte at
       record+0xAD4: 0 integrates the eye position forward along the polar
       step, 2 integrates it backward, 1 only recomputes angle/distance.
       Rounds 43/44 measured two smooth interleaved tracks alternating every
       frame in one record, which is the exact signature of the mode byte
       flipping between the forward and reverse cases. Sample it beside the
       accumulator and the committed value it feeds. */
    {
        int mode[5];
        size_t slot;

        for (slot = 0u; slot < 5u; ++slot) {
            mode[slot] =
                watch_byte((uint32_t)(0x0041A800u + slot * 0x0AE0u + 0xAD4u));
        }
        fprintf(
            stderr,
            "recomp watch:   cammode %d %d %d %d %d acc=%g,%g,%g"
            " tgt=%g,%g,%g\n",
            mode[0], mode[1], mode[2], mode[3], mode[4],
            (double)watch_float(0x0041A910u),
            (double)watch_float(0x0041A914u),
            (double)watch_float(0x0041A918u),
            (double)watch_float(0x0041A920u),
            (double)watch_float(0x0041A924u),
            (double)watch_float(0x0041A928u));
    }
    /* Round 47: sub_0002FE80 mode 1 is a lerp toward the target whose gain is
       MEMF(0x281250) / record+0x13C * MEMF(0x281458). Rounds 45/46 measured the
       accumulator alternating by a factor of about 4 with a constant target and
       a constant mode, which can only come from the divisor at +0x13C changing
       between frames. Sample the counter, the two enable flags it is gated on,
       and the two constants. */
    fprintf(
        stderr,
        "recomp watch:   camlerp ctr=%g f424=%d f426=%d k1250=%g k1458=%g"
        " ang=%d dist=%g\n",
        (double)watch_float(0x0041A93Cu),
        watch_byte(0x0041AC24u),
        watch_byte(0x0041AC26u),
        (double)watch_float(0x00281250u),
        (double)watch_float(0x00281458u),
        watch_byte(0x0041A920u),
        (double)watch_float(0x0041A92Cu));
    fprintf(
        stderr,
        "recomp watch:   camguard aca=%d acc=%d ad8=%d ang=%d,%d dist=%g"
        " head=%g,%g,%g\n",
        watch_byte(0x0041B2CAu),
        watch_byte(0x0041B2CCu),
        watch_byte(0x0041B2D8u),
        watch_byte(0x0041A930u),
        watch_byte(0x0041A931u),
        (double)watch_float(0x0041A93Cu),
        (double)watch_float(0x0041A800u),
        (double)watch_float(0x0041A804u),
        (double)watch_float(0x0041A808u));
    /* Round 49: record+0x130 (the polar angle) alternates between exactly two
       values, 0x3FEE and 0x3F99, while distance is smooth and record+0xACA is 0.
       sub_000301C0 copies +0x130 from a staging block at +0x168 and is gated on
       record+0xAD2. Sample the gate, the staging angle, and the staging position
       at +0x158, to see whether the alternation is already present upstream. */
    fprintf(
        stderr,
        "recomp watch:   camstage ad2=%d ad4=%d stage_ang=%d,%d stage_pos=%g,%g,%g"
        " stage_dist=%g cur_ang=%d,%d\n",
        watch_byte(0x0041B2D2u),
        watch_byte(0x0041B2D4u),
        watch_byte(0x0041A968u),
        watch_byte(0x0041A969u),
        (double)watch_float(0x0041A958u),
        (double)watch_float(0x0041A95Cu),
        (double)watch_float(0x0041A960u),
        (double)watch_float(0x0041A974u),
        watch_byte(0x0041A930u),
        watch_byte(0x0041A931u));
    /* Round 50: mode 1 of sub_0002DFE0 recomputes the angle at +0x130 from the
       difference between the accumulator at +0x110 and the block at esi+0x10,
       which with esi = record+0x110 is +0x120. Rounds 45-47 read +0x120 as the
       constant target (0,5,0). Sample the whole +0x110..+0x12C span so the
       aliasing question is answered from one line. */
    fprintf(
        stderr,
        "recomp watch:   camspan p=%g,%g,%g q=%g,%g,%g ang=%d,%d a24=%d,%d"
        " dist=%g\n",
        (double)watch_float(0x0041A910u),
        (double)watch_float(0x0041A914u),
        (double)watch_float(0x0041A918u),
        (double)watch_float(0x0041A920u),
        (double)watch_float(0x0041A924u),
        (double)watch_float(0x0041A928u),
        watch_byte(0x0041A930u),
        watch_byte(0x0041A931u),
        watch_byte(0x0041A934u),
        watch_byte(0x0041A935u),
        (double)watch_float(0x0041A93Cu));
    /* Round 51: +0x120 took zero writes across a whole run, so there is no
       feedback loop and the target is genuinely constant. sub_000301C0 does run
       (its gate record+0xAD2 measured 1) and copies a staging block at +0x148
       into the accumulator at +0x110 (ebx = ebp+0x110, edi = ebp+0x148). Rounds
       49/50 measured the other two staging blocks, +0x158 and +0x168, and both
       are constant. +0x148 is the one block on that path never sampled. */
    fprintf(
        stderr,
        "recomp watch:   camstage2 s148=%g,%g,%g s140=%g s144=%g s17c=%g"
        " s178=%g acc=%g,%g,%g\n",
        (double)watch_float(0x0041A948u),
        (double)watch_float(0x0041A94Cu),
        (double)watch_float(0x0041A950u),
        (double)watch_float(0x0041A940u),
        (double)watch_float(0x0041A944u),
        (double)watch_float(0x0041A97Cu),
        (double)watch_float(0x0041A978u),
        (double)watch_float(0x0041A910u),
        (double)watch_float(0x0041A914u),
        (double)watch_float(0x0041A918u));
    /* Round 52: sub_000301C0 selects its branch on two record bytes read back to
       back at recomp_0001_ebp.c:13642 and 13645 - +0xAD3 gates the whole copy
       block and +0xAD5 chooses which staging block feeds it. Round 51 showed the
       orbit at +0x148 is clean while the accumulator alternates, so if one of
       these selectors alternates it is the defect. Sample the whole control-byte
       row +0xAD0..+0xAD9 in one line. */
    fprintf(
        stderr,
        "recomp watch:   camsel ad0=%d ad1=%d ad2=%d ad3=%d ad4=%d ad5=%d"
        " ad6=%d ad7=%d ad8=%d ad9=%d\n",
        watch_byte(0x0041B2D0u),
        watch_byte(0x0041B2D1u),
        watch_byte(0x0041B2D2u),
        watch_byte(0x0041B2D3u),
        watch_byte(0x0041B2D4u),
        watch_byte(0x0041B2D5u),
        watch_byte(0x0041B2D6u),
        watch_byte(0x0041B2D7u),
        watch_byte(0x0041B2D8u),
        watch_byte(0x0041B2D9u));
    /* Round 55: round 54 proved the placement half is lossless (elevation of
       the position matches the previous frame's angle to 0.0001 deg) while the
       angle alternates in an exact period-2 limit cycle. With +0xAD3 and +0xAD5
       both 0 the driver takes loc_00030611, which computes atan2 of staging
       +0x158/+0x160 minus center +0x120/+0x128 and converts it with the factor
       at 0x281284. That delta is the only unmeasured input on the taken path.
       Sample both endpoints and the derived delta. */
    fprintf(
        stderr,
        "recomp watch:   camatan s158=%.9g s15c=%.9g s160=%.9g"
        " c120=%.9g c124=%.9g c128=%.9g dx=%.9g dz=%.9g k=%.9g\n",
        (double)watch_float(0x0041A958u),
        (double)watch_float(0x0041A95Cu),
        (double)watch_float(0x0041A960u),
        (double)watch_float(0x0041A920u),
        (double)watch_float(0x0041A924u),
        (double)watch_float(0x0041A928u),
        (double)(watch_float(0x0041A958u) - watch_float(0x0041A920u)),
       (double)(watch_float(0x0041A960u) - watch_float(0x0041A928u)),
       (double)watch_float(0x00281284u));
    /* Round 56: sub_0002EF54 branches on the global byte 0x41EFF0. When it is
       zero the loop calls sub_000301C0 (the integrator driver) then sub_0002FE80
       then the commit; when it is nonzero it calls sub_0002FE80 and the commit
       ONLY, skipping the integrator entirely. If that byte alternates, the
       camera integrates on every other frame, which is an exact period-2 source
       that needs no numerical error at all. Sample it beside the live block, the
       committed block, and both angles. */
    fprintf(
        stderr,
        "recomp watch:   camloop eff0=%d ad6=%d live=%.9g,%.9g,%.9g"
        " cmt=%.9g,%.9g,%.9g ang=%d cang=%d head=%.9g,%.9g,%.9g\n",
        watch_byte(0x0041EFF0u),
        watch_byte(0x0041B2D6u),
        (double)watch_float(0x0041A910u),
        (double)watch_float(0x0041A914u),
        (double)watch_float(0x0041A918u),
        (double)watch_float(0x0041A980u),
        (double)watch_float(0x0041A984u),
        (double)watch_float(0x0041A988u),
        watch_byte(0x0041A930u) | (watch_byte(0x0041A931u) << 8),
        watch_byte(0x0041A9A0u) | (watch_byte(0x0041A9A1u) << 8),
       (double)watch_float(0x0041A800u),
       (double)watch_float(0x0041A804u),
       (double)watch_float(0x0041A808u));
    /* Round 57: sub_00022B40 is an angle slew limiter. sub_000301C0 calls it at
       lines 13976/13986/13996 with ecx loaded from ebp+0x168/+0x16C/+0x170 (the
       family-B staging angle), edx from ebp+0x210/+0x214/+0x218 (deadband) and a
       rate filed from ebp+0x1D8/+0x1DC/+0x1E0. Round 56 identified a stale-target
       feedback delay as the oscillator. Pre-registered falsifier: if the staging
       target +0x168 is constant while the live angle alternates, the target is
       not the stale committed block and the drive is elsewhere. */
    fprintf(
        stderr,
        "recomp watch:   camslew tgt=%d,%d,%d band=%d,%d rate=%d,%d"
        " ang=%d cang=%d d13c=%.9g\n",
        watch_byte(0x0041A968u) | (watch_byte(0x0041A969u) << 8),
        watch_byte(0x0041A96Cu) | (watch_byte(0x0041A96Du) << 8),
        watch_byte(0x0041A970u) | (watch_byte(0x0041A971u) << 8),
        watch_byte(0x0041AA10u) | (watch_byte(0x0041AA11u) << 8),
        watch_byte(0x0041AA14u) | (watch_byte(0x0041AA15u) << 8),
        watch_byte(0x0041A9D8u) | (watch_byte(0x0041A9D9u) << 8),
        watch_byte(0x0041A9DCu) | (watch_byte(0x0041A9DDu) << 8),
        watch_byte(0x0041A930u) | (watch_byte(0x0041A931u) << 8),
        watch_byte(0x0041A9A0u) | (watch_byte(0x0041A9A1u) << 8),
        (double)watch_float(0x0041A93Cu));
    /* Round 59: the driver sub_000301C0 reloads ebx from MEM32(esp + 0x3C) at
       recomp_0001_ebp.c:14023, immediately before esp = esp + 0x24, and that ebx
       becomes the mode-0 placement target at 14055 (esi = ebx). ebx was saved at
       13558 via MEM32(esp + 0x18) = ebx. Between the two there is a PUSH32 at
       13970 with no matching pop found on the taken path, so if esp is off by 4
       at 14023 the reload picks up a neighbouring slot and mode 0 writes a
       different block than mode 1 read from - lossless, out of phase, exactly
       the observed 2-cycle. The watch fires from recomp_memory() while the guest
       registers are still live, so sampling them here names the pointer the
       integrator was handed. Pre-registered falsifier: if esi equals 0x0041A910
       (record 0 + 0x110) on every hit, the stack-slot theory is dead. */
    fprintf(
        stderr,
        "recomp watch:   camebx esi=0x%08" PRIx32 " ebx=0x%08" PRIx32
        " esp=0x%08" PRIx32 " ebp=0x%08" PRIx32 " edi=0x%08" PRIx32
        " eax=%" PRIu32 "\n",
        recomp_runtime.registers.esi,
        recomp_runtime.registers.ebx,
        recomp_runtime.registers.esp,
        recomp_runtime.registers.ebp,
        recomp_runtime.registers.edi,
        recomp_runtime.registers.eax);
    /* Round 61: the r60 register signature at the steady-state angle write is
       esi=0x0041A910, edi=0x0041A800, ebx=0x0041EFD8, which is sub_00030070 line
       13421 (esi = edi + 0x110) called from sub_0002EF54 (ebx = 0x41EFD8 at
       11444) - the FOURTH integrator call, inside the commit, not the driver.
       Mode 1 recomputes pitch at recomp_0001_ebp.c:9192-9198 as
       atan2(dy, sqrt(dx*dx + dz*dz)). But lines 9175-9180 first compare the
       horizontal sum against MEMF(0x28124C) and, when it compares below,
       overwrite the operand with 1.0f (0x3F800000). That substitution has NO
       counterpart on the yaw path, which is computed straight from atan2(dx, dz)
       at 9155 - and yaw is the component that does not oscillate. Measured h
       alternates 0.43 and 3.51, so h*h alternates 0.19 and 12.3; if the threshold
       falls between them the clamp engages on alternate frames only. Sample the
       deltas, the horizontal sum, and the threshold. Falsifier: if the threshold
       is below both values of h*h the clamp never engages and this is dead. */
    fprintf(
        stderr,
        "recomp watch:   camclamp dx=%.9g dy=%.9g dz=%.9g h2=%.9g"
        " thr=%.9g clamped=%d\n",
        (double)(watch_float(0x0041A910u) - watch_float(0x0041A920u)),
        (double)(watch_float(0x0041A914u) - watch_float(0x0041A924u)),
        (double)(watch_float(0x0041A918u) - watch_float(0x0041A928u)),
        (double)((watch_float(0x0041A910u) - watch_float(0x0041A920u)) *
                 (watch_float(0x0041A910u) - watch_float(0x0041A920u)) +
                 (watch_float(0x0041A918u) - watch_float(0x0041A928u)) *
                 (watch_float(0x0041A918u) - watch_float(0x0041A928u))),
        (double)watch_float(0x0028124Cu),
        (int)(((watch_float(0x0041A910u) - watch_float(0x0041A920u)) *
               (watch_float(0x0041A910u) - watch_float(0x0041A920u)) +
               (watch_float(0x0041A918u) - watch_float(0x0041A928u)) *
              (watch_float(0x0041A918u) - watch_float(0x0041A928u)))
             < watch_float(0x0028124Cu)));
    /* Round 62: r61 proved placement and recovery are mutually exact, and the
       mode byte +0xAD4 is constant 1 - the half that ONLY recomputes the angle and
       never writes position. Yet position moves every frame. So a non-integrator
       writer owns +0x110/114/118. Candidates: sub_0002FE80 (the lerp, writes
       ecx+0x110/114/118 at recomp_0001_ebp.c:13250/13256/13262, gated on +0x424
       and +0x426), sub_0002FFB0 (writes esi+0/4/8 with esi = edi+0x180 at
       13363-13368), and sub_00024E20 (zeroes +0x130 unconditionally at
       recomp_0000_ebp.c:47505 and reloads +0x120 from the table at 0x4D6720
       indexed by +0xAD0). Sample the lerp gates, the reset gates, the table index,
       and the two staging angles that feed the reset path. Falsifier: if +0x424
       and +0x426 are both constant and +0xACA/+0xACB never change, neither the
       lerp nor the reset is toggling and the writer is sub_0002FFB0. */
    fprintf(
        stderr,
        "recomp watch:   camwriter g424=%d g426=%d g425=%d aca=%d acb=%d acf=%d"
        " ad0=%d ad4=%d d13c=%.9g s5c8=%d k1458=%.9g k1250=%.9g\n",
        watch_byte(0x0041AC24u),
        watch_byte(0x0041AC26u),
        watch_byte(0x0041AC25u),
        watch_byte(0x0041B2CAu),
        watch_byte(0x0041B2CBu),
        watch_byte(0x0041B2CFu),
        watch_byte(0x0041B2D0u),
        watch_byte(0x0041B2D4u),
        (double)watch_float(0x0041A93Cu),
        watch_byte(0x0041ADC8u),
        (double)watch_float(0x00281458u),
        (double)watch_float(0x00281250u));
    /* Round 69: r68 proved the loop is closed - mode 0 places position from the
       pitch at esi+0x20 (recomp_0001_ebp.c:9227) scaled by esi+0x2C (9236/9259),
       and mode 1 recovers that pitch from the position with atan2 (9192) and
       writes esi+0x2C from sub_0017A970 at 9221. Both run on the live block every
       frame, so the state is a closed cycle of two mutually inverse maps. A
       correct inverse pair is a no-op, so the halves must disagree about a
       parameter. Measured h swings by 9.2x while dy is fixed, and mode 0 scales by
       +0x2C while the driver's own distance lives at +0x13C. If those two fields
       differ, the halves are using different radii. Falsifier: if +0x2C alternates
       while +0x13C stays smooth, the 2-cycle is a radius disagreement and the fix
       is local to one field; if both are smooth, the disagreement is in the trig
       argument instead. Also sample the yaw at +0x134 and the recovered yaw at
       +0x24, which should be equal if the pair is consistent. */
    fprintf(
        stderr,
        "recomp watch:   camradius r2c=%.9g r13c=%.9g pitch20=%d yaw24=%d"
        " pitch130=%d yaw134=%d cx=%.9g cy=%.9g cz=%.9g\n",
        (double)watch_float(0x0041A92Cu),
        (double)watch_float(0x0041A93Cu),
        watch_byte(0x0041A930u) | (watch_byte(0x0041A931u) << 8),
        watch_byte(0x0041A934u) | (watch_byte(0x0041A935u) << 8),
        watch_byte(0x0041A930u) | (watch_byte(0x0041A931u) << 8),
        watch_byte(0x0041A934u) | (watch_byte(0x0041A935u) << 8),
        (double)watch_float(0x0041A920u),
        (double)watch_float(0x0041A924u),
        (double)watch_float(0x0041A928u));
   for (depth = 0u; depth < 6u; ++depth) {
       uint32_t target;
       const char *member;
        int line;

        if (!recomp_dispatch_frame_at(depth, &target, &member, &line)) {
            break;
        }
        fprintf(
            stderr,
            "recomp watch:   frame %zu target=0x%08" PRIx32 " site=%s:%d\n",
            depth,
            target,
            member != NULL ? member : "<adapter>",
            line);
    }
}

static void check_watch_write(void)
{
    const uint8_t *data;
    uint32_t current;

    if (!watch.active) {
        return;
    }

    data = find_memory_region(watch.address, sizeof(uint32_t));
    if (data == NULL) {
        return;
    }
    memcpy(&current, data, sizeof current);
    if (!watch.armed) {
        watch.armed = true;
        watch.value = current;
        return;
    }
    if (current == watch.value) {
        return;
    }

    ++watch.hits;
    if (watch.hits <= watch.hit_limit) {
        report_watch_writer(watch.value, current);
    } else if (watch.hits == watch.hit_limit + 1u) {
        fprintf(stderr, "recomp watch: hit limit reached, silencing\n");
    }
    watch.value = current;
}

/* The full runner maps one contiguous RAM allocation. Keep instrumented and
   device accesses on the general path, including the preceding OHCI flush. */
static uint8_t *find_direct_ram(uint32_t guest_address, size_t width)
{
    uint32_t window = guest_address & 0xfc000000u;
    uint32_t offset = guest_address & 0x03ffffffu;

    if (direct_ram == NULL || watch.active || recomp_runtime.accesses != NULL ||
        (window != 0u && window != XBOX_CACHED_ALIAS &&
         window != XBOX_PHYSICAL_ALIAS) || width > XBOX_RAM_SIZE - offset) {
        return NULL;
    }
    return direct_ram + offset;
}

/* Keep device and diagnostic work out of the inlined RAM accessors. */
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
static uint8_t *recomp_memory_slow(uint32_t guest_address, size_t width)
{
    uint8_t *memory;

    check_watch_write();

    /* MCPX audio aperture is device space, not guest RAM, so it is served
       from its model instead of a declared region. */
    memory = recomp_apu_access(&mcpx_apu, guest_address, width);
    if (memory != NULL) {
        record_memory_access(guest_address, width);
        return memory;
    }

    memory = find_memory_region(guest_address, width);
    if (memory == NULL && guest_address >= XBOX_CACHED_ALIAS &&
        guest_address - XBOX_CACHED_ALIAS < XBOX_RAM_SIZE) {
        memory = find_memory_region(
            guest_address - XBOX_CACHED_ALIAS, width);
    }
    /* Xbox D3D returns this physical-memory view for D3DLOCK_TILED. */
    if (memory == NULL && guest_address >= XBOX_PHYSICAL_ALIAS &&
        guest_address - XBOX_PHYSICAL_ALIAS < XBOX_RAM_SIZE) {
        memory = find_memory_region(
            guest_address - XBOX_PHYSICAL_ALIAS, width);
    }
    if (memory != NULL) {
        record_memory_access(guest_address, width);
        return memory;
    }

    fail_memory_access(guest_address, width);
    return NULL;
}

uint8_t *recomp_memory(uint32_t guest_address, size_t width)
{
    uint8_t *memory;

    if (mmio_u32_access.pending) {
        commit_mmio_u32_write();
    }
    memory = find_direct_ram(guest_address, width);
    if (memory != NULL) {
        return memory;
    }
    return recomp_memory_slow(guest_address, width);
}

uint32_t *recomp_memory_u32_checked(uint32_t guest_address)
{
    RecompOhciRegister reg;
    uint32_t value;
    uint8_t *memory;

    if (mmio_u32_access.pending) {
        commit_mmio_u32_write();
    }
    memory = find_direct_ram(guest_address, sizeof(uint32_t));
    if (memory != NULL) {
        return (uint32_t *)(void *)memory;
    }
    if (usb0_ohci_register(guest_address, &reg)) {
        if (!recomp_ohci_read(&usb0_ohci, reg, &value)) {
            fail_memory_access(guest_address, sizeof(uint32_t));
            return NULL;
        }
        record_memory_access(guest_address, sizeof(uint32_t));
        mmio_u32_access = (RecompMmioU32Access){
            .pending = true,
            .guest_address = guest_address,
            .reg = reg,
            .original = value,
            .value = value,
        };
        update_fast_ram();
        return &mmio_u32_access.value;
    }

    return (uint32_t *)(void *)recomp_memory(
        guest_address, sizeof(uint32_t));
}

uint64_t *recomp_memory_u64(uint32_t guest_address)
{
    return (uint64_t *)(void *)recomp_memory(
        guest_address, sizeof(uint64_t));
}

uint16_t *recomp_memory_u16(uint32_t guest_address)
{
    return (uint16_t *)(void *)recomp_memory(
        guest_address, sizeof(uint16_t));
}

int8_t *recomp_memory_i8(uint32_t guest_address)
{
    return (int8_t *)(void *)recomp_memory(guest_address, sizeof(int8_t));
}

void recomp_guest_memcpy(
    uint32_t destination,
    uint32_t source,
    size_t size)
{
    uint8_t *destination_data;
    const uint8_t *source_data;

    if (size == 0u) {
        return;
    }

    source_data = recomp_memory(source, size);
    destination_data = recomp_memory(destination, size);
    memcpy(destination_data, source_data, size);
}

void recomp_guest_memset(
    uint32_t destination,
    int value,
    size_t size)
{
    uint8_t *destination_data;

    if (size == 0u) {
        return;
    }

    destination_data = recomp_memory(destination, size);
    memset(destination_data, value, size);
}

void recomp_guest_load(void *destination, uint32_t source, size_t size)
{
    const uint8_t *source_data;

    if (size == 0u) {
        return;
    }

    source_data = recomp_memory(source, size);
    memcpy(destination, source_data, size);
}

void recomp_guest_store(uint32_t destination, const void *source, size_t size)
{
    uint8_t *destination_data;

    if (size == 0u) {
        return;
    }

    destination_data = recomp_memory(destination, size);
    memcpy(destination_data, source, size);
}

uint32_t *recomp_ebp_register(void)
{
    return &recomp_runtime.registers.ebp;
}

uint32_t *recomp_esp_register(void)
{
    return &recomp_runtime.registers.esp;
}

bool recomp_dispatch(uint32_t guest_address)
{
    /* ponytail: linear lookup; sort and binary-search when table size matters. */
    for (size_t i = 0; i < recomp_runtime.function_count; ++i) {
        const RecompFunctionEntry *entry = &recomp_runtime.functions[i];

        if (entry->address == guest_address && entry->function != NULL) {
            recomp_last_dispatch_address = guest_address;
            entry->function();
            return true;
        }
    }

    if (recomp_runtime.lookup != NULL) {
        RecompFunction function = recomp_runtime.lookup(guest_address);

        if (function != NULL) {
            recomp_last_dispatch_address = guest_address;
            function();
            return true;
        }
    }

    return false;
}

static const char *member_name(const char *member)
{
    const char *name = member;

    for (const char *cursor = member; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            name = cursor + 1;
        }
    }
    return name;
}

static void copy_token(char *out, size_t size, const char *text)
{
    size_t i = 0;

    while (i + 1u < size && text[i] != '\0' && text[i] != ':' &&
           text[i] != '(' && text[i] != ' ' && text[i] != '\r' &&
           text[i] != '\n') {
        out[i] = text[i];
        ++i;
    }
    out[i] = '\0';
}

/* Names the generated site by rescanning the compiled member for the last
   guest label above the reported line; generated members carry no line map. */
static void generated_site(
    const char *member,
    int line,
    char *function,
    size_t function_size,
    char *label,
    size_t label_size)
{
    FILE *source = fopen(member, "rb");
    char text[512];
    int line_number = 1;
    int at_line_start = 1;

    function[0] = '\0';
    label[0] = '\0';
    if (source == NULL) {
        return;
    }

    while (line_number <= line && fgets(text, sizeof text, source) != NULL) {
        size_t length;

        if (at_line_start) {
            if (strncmp(text, "loc_", 4) == 0) {
                copy_token(label, label_size, text);
            } else if (strncmp(text, "void sub_", 9) == 0) {
                copy_token(function, function_size, text + 5);
                label[0] = '\0';
            }
        }
        length = strlen(text);
        at_line_start = length > 0u && text[length - 1u] == '\n';
        if (at_line_start) {
            ++line_number;
        }
    }
    fclose(source);
}

void recomp_generated_breakpoint(const char *member, int line)
{
    char function[64];
    char label[64];

    generated_site(member, line, function, sizeof function, label, sizeof label);
    fprintf(
        stderr,
        "recomp runtime: generated breakpoint at %s:%d function=%s label=%s"
        " esp=0x%08" PRIx32 " ebp=0x%08" PRIx32 " dispatch=0x%08" PRIx32 "\n",
        member_name(member),
        line,
        function[0] != '\0' ? function : "<unknown>",
        label[0] != '\0' ? label : "<unknown>",
        recomp_runtime.registers.esp,
        recomp_runtime.registers.ebp,
        recomp_last_dispatch_address);
    recomp_stop(2, "breakpoint:%s", member_name(member));
}

/* Innermost frame first: the top entry is the function that faulted, and the
   rest is the guest call path that reached it. */
static void report_dispatch_stack(void)
{
    size_t capacity = sizeof dispatch_stack / sizeof dispatch_stack[0];
    size_t depth = dispatch_depth < capacity ? dispatch_depth : capacity;
    size_t reported = 0;

    while (depth > 0u && reported < 12u) {
        const RecompDispatchFrame *frame = &dispatch_stack[--depth];
        char function[64];
        char label[64];

        if (frame->member == NULL) {
            fprintf(
                stderr,
                "recomp runtime: dispatch frame %zu target=0x%08" PRIx32
                " site=<adapter>\n",
                reported,
                frame->guest_address);
            ++reported;
            continue;
        }
        generated_site(
            frame->member,
            frame->line,
            function,
            sizeof function,
            label,
            sizeof label);
        fprintf(
            stderr,
            "recomp runtime: dispatch frame %zu target=0x%08" PRIx32
            " site=%s:%d function=%s label=%s\n",
            reported,
            frame->guest_address,
            member_name(frame->member),
            frame->line,
            function[0] != '\0' ? function : "<unknown>",
            label[0] != '\0' ? label : "<unknown>");
        ++reported;
    }
}

static void recomp_dispatch_indirect_from(
    uint32_t guest_address,
    uint32_t saved_esp,
    const char *member,
    int line)
{
    if (dispatch_depth < sizeof dispatch_stack / sizeof dispatch_stack[0]) {
        dispatch_stack[dispatch_depth] = (RecompDispatchFrame){
            .guest_address = guest_address,
            .member = member,
            .line = line,
        };
    }
    ++dispatch_depth;
    if (recomp_dispatch(guest_address)) {
        --dispatch_depth;
        return;
    }
    --dispatch_depth;

    /* Encoded kernel thunks read as 0x8000xxxx; name the missing import. */
    if ((guest_address & 0xffff0000u) == 0x80000000u) {
        uint32_t ordinal = guest_address & 0xffffu;
        const char *name = recomp_kernel_ordinal_name(ordinal);

        fprintf(
            stderr,
            "recomp runtime: unimplemented kernel import ordinal %" PRIu32
            " (%s) at saved esp 0x%08" PRIx32 "\n",
            ordinal,
            name != NULL ? name : "not imported by this XBE",
            saved_esp);
        recomp_stop(
            2, "import:%s", name != NULL ? name : "unknown");
    }

    fprintf(
        stderr,
        "recomp runtime: unresolved indirect call to 0x%08" PRIx32
        " at saved esp 0x%08" PRIx32 "\n",
        guest_address,
        saved_esp);
    fprintf(
        stderr,
        "recomp runtime: unresolved indirect registers eax=0x%08" PRIx32
        " ecx=0x%08" PRIx32 " edx=0x%08" PRIx32
        " ebx=0x%08" PRIx32 " esi=0x%08" PRIx32
        " edi=0x%08" PRIx32 "\n",
        recomp_runtime.registers.eax,
        recomp_runtime.registers.ecx,
        recomp_runtime.registers.edx,
        recomp_runtime.registers.ebx,
        recomp_runtime.registers.esi,
        recomp_runtime.registers.edi);
    if (member != NULL) {
        fprintf(
            stderr,
            "recomp runtime: unresolved indirect site %s:%d\n",
            member_name(member),
            line);
    }
    recomp_stop(2, "indirect:0x%08" PRIx32, guest_address);
}

void recomp_dispatch_indirect(uint32_t guest_address, uint32_t saved_esp)
{
    recomp_dispatch_indirect_from(guest_address, saved_esp, NULL, 0);
}

void recomp_dispatch_indirect_site(
    uint32_t guest_address,
    uint32_t saved_esp,
    const char *member,
    int line)
{
    recomp_dispatch_indirect_from(guest_address, saved_esp, member, line);
}

bool recomp_dispatch_frame_at(
    size_t depth,
    uint32_t *guest_address,
    const char **member,
    int *line)
{
    size_t capacity = sizeof dispatch_stack / sizeof dispatch_stack[0];
    size_t live = dispatch_depth < capacity ? dispatch_depth : capacity;
    const RecompDispatchFrame *frame;

    if (depth >= live) {
        return false;
    }
    /* Depth 0 is the innermost frame, matching how a caller thinks about a
       backtrace, while the array grows outward from index 0. */
    frame = &dispatch_stack[live - 1u - depth];
    if (guest_address != NULL) {
        *guest_address = frame->guest_address;
    }
    if (member != NULL) {
        *member = frame->member;
    }
    if (line != NULL) {
        *line = frame->line;
    }
    return true;
}

void recomp_fpu_context_save(RecompFpuContext *context)
{
    if (context == NULL) {
        return;
    }
    memcpy(context->xmm, recomp_runtime.xmm, sizeof context->xmm);
    memcpy(
        context->fpu_stack,
        recomp_runtime.fpu_stack,
        sizeof context->fpu_stack);
    context->fpu_top = recomp_runtime.fpu_top;
    context->fpu_control_word = recomp_runtime.fpu_control_word;
    context->fpu_compare = recomp_runtime.fpu_compare;
}

void recomp_fpu_context_restore(const RecompFpuContext *context)
{
    if (context == NULL) {
        return;
    }
    memcpy(recomp_runtime.xmm, context->xmm, sizeof recomp_runtime.xmm);
    memcpy(
        recomp_runtime.fpu_stack,
        context->fpu_stack,
        sizeof recomp_runtime.fpu_stack);
    recomp_runtime.fpu_top = context->fpu_top;
    recomp_runtime.fpu_control_word = context->fpu_control_word;
    recomp_runtime.fpu_compare = context->fpu_compare;
}
