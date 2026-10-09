#include "kernel_abi.h"
#include "xbox_memory_layout.h"
#ifdef RECOMP_D3D_FRAME_ENABLED
#include "d3d_frame_adapter.h"
#include "stop_report.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
    PAGE_READWRITE = 0x04u,
    MEM_RELEASE = 0x8000u,
    MAX_ALLOCATIONS = 4096u,
};

static const uint32_t STATUS_SUCCESS = 0x00000000u;
static const uint32_t STATUS_NOT_IMPLEMENTED = 0xc0000002u;
static const uint32_t STATUS_INVALID_PARAMETER = 0xc000000du;
static const uint32_t STATUS_NO_MEMORY = 0xc0000017u;

typedef enum AllocationKind {
    ALLOCATION_POOL,
    ALLOCATION_CONTIGUOUS,
    ALLOCATION_SYSTEM,
    ALLOCATION_STACK,
    ALLOCATION_VIRTUAL,
} AllocationKind;

typedef struct Allocation {
    uint32_t base;
    uint32_t size;
    uint32_t capacity;
    uint32_t protect;
    AllocationKind kind;
    int active;
} Allocation;

static Allocation allocations[MAX_ALLOCATIONS];

static uint32_t effective_size(uint32_t size)
{
    return size == 0u ? 1u : size;
}

static int guest_range_is_mapped(uint32_t base, uint32_t size)
{
    uint64_t end = (uint64_t)base + size;

    for (size_t i = 0; i < recomp_runtime.memory_region_count; ++i) {
        const RecompMemoryRegion *region = &recomp_runtime.memory_regions[i];
        uint64_t region_end = (uint64_t)region->address + region->size;

        if (base >= region->address && end <= region_end) {
            return 1;
        }
    }
    return 0;
}

static Allocation *find_allocation(uint32_t base)
{
    for (size_t i = 0; i < MAX_ALLOCATIONS; ++i) {
        if (allocations[i].active && allocations[i].base == base) {
            return &allocations[i];
        }
    }
    return NULL;
}

static Allocation *find_containing_allocation(uint32_t address)
{
    for (size_t i = 0; i < MAX_ALLOCATIONS; ++i) {
        uint64_t end = (uint64_t)allocations[i].base + allocations[i].size;

        if (allocations[i].active && address >= allocations[i].base &&
            address < end) {
            return &allocations[i];
        }
    }
    return NULL;
}

static Allocation *track_allocation(
    uint32_t base,
    uint32_t size,
    uint32_t protect,
    AllocationKind kind)
{
    for (size_t i = 0; i < MAX_ALLOCATIONS; ++i) {
        if (allocations[i].base == 0u && !allocations[i].active) {
            allocations[i] = (Allocation){
                .base = base,
                .size = size,
                .protect = protect,
                .kind = kind,
                .active = 1,
            };
            return &allocations[i];
        }
    }
    return NULL;
}

static Allocation *reuse_allocation(
    uint32_t capacity, uint32_t alignment, uint32_t lowest, uint32_t highest,
    AllocationKind kind)
{
    Allocation *best = NULL;
    for (size_t i = 0; i < MAX_ALLOCATIONS; ++i) {
        Allocation *candidate = &allocations[i];
        uint32_t physical = candidate->base & 0x7fffffffu;
        if (!candidate->active && candidate->capacity >= capacity &&
            (candidate->kind == ALLOCATION_CONTIGUOUS) ==
                (kind == ALLOCATION_CONTIGUOUS) &&
            (physical & (alignment - 1u)) == 0u && physical >= lowest &&
            (uint64_t)physical + capacity <= (uint64_t)highest + 1u &&
            (best == NULL || candidate->capacity < best->capacity)) {
            best = candidate;
        }
    }
    if (best != NULL && best->capacity > capacity) {
        /* If metadata is full, retain the remainder until this block is freed. */
        for (size_t i = 0; i < MAX_ALLOCATIONS; ++i) {
            if (allocations[i].base == 0u && !allocations[i].active) {
                allocations[i] = (Allocation){
                    .base = best->base + capacity,
                    .capacity = best->capacity - capacity,
                    .kind = best->kind,
                };
                best->capacity = capacity;
                break;
            }
        }
    }
    return best;
}

static uint32_t allocate_range(
    uint32_t size,
    uint32_t alignment,
    uint32_t protect,
    AllocationKind kind,
    uint32_t lowest,
    uint32_t highest)
{
    uint32_t actual_size = effective_size(size);
    uint32_t granularity = kind == ALLOCATION_CONTIGUOUS ? 0x1000u : 4u;
    uint64_t rounded = ((uint64_t)actual_size + granularity - 1u) &
        ~(uint64_t)(granularity - 1u);
    if (alignment < granularity) alignment = granularity;
    if ((alignment & (alignment - 1u)) != 0u || rounded > UINT32_MAX ||
        lowest > highest) {
        return 0u;
    }
    uint32_t capacity = (uint32_t)rounded;
    Allocation *allocation = reuse_allocation(capacity, alignment, lowest, highest, kind);
    if (allocation == NULL) {
        /* Reserve bookkeeping before consuming the monotonic backing arena. */
        allocation = track_allocation(0u, 0u, protect, kind);
        if (allocation == NULL) return 0u;
        uint32_t base = kind == ALLOCATION_CONTIGUOUS
            ? xbox_ContiguousAlloc(capacity, lowest, highest, alignment)
            : xbox_HeapAlloc(capacity, alignment);
        if (base == 0u) {
            *allocation = (Allocation){0};
            return 0u;
        }
        allocation->base = base;
        allocation->capacity = capacity;
    }
    allocation->size = actual_size;
    allocation->protect = protect;
    allocation->kind = kind;
    allocation->active = 1;
    recomp_guest_memset(allocation->base, 0, actual_size);
    return allocation->base;
}

static uint32_t allocate_guest(
    uint32_t size, uint32_t alignment, uint32_t protect, AllocationKind kind)
{
    return allocate_range(size, alignment, protect, kind, 0u, UINT32_MAX);
}

static uint32_t allocate_contiguous_guest(
    uint32_t size,
    uint32_t lowest_address,
    uint32_t highest_address,
    uint32_t alignment,
    uint32_t protect)
{
    return allocate_range(size, alignment, protect, ALLOCATION_CONTIGUOUS,
        lowest_address, highest_address);
}

static void coalesce_free_allocation(Allocation *allocation)
{
    for (size_t i = 0; i < MAX_ALLOCATIONS; ++i) {
        Allocation *other = &allocations[i];
        if (other == allocation || other->active || other->capacity == 0u ||
            (other->kind == ALLOCATION_CONTIGUOUS) !=
                (allocation->kind == ALLOCATION_CONTIGUOUS)) {
            continue;
        }
        if ((uint64_t)other->base + other->capacity == allocation->base) {
            allocation->base = other->base;
        } else if ((uint64_t)allocation->base + allocation->capacity != other->base) {
            continue;
        }
        allocation->capacity += other->capacity;
        *other = (Allocation){0};
        i = (size_t)-1; /* The larger range may now meet an earlier free block. */
    }
}

static void free_guest(uint32_t base)
{
    Allocation *allocation = find_allocation(base);

    if (allocation != NULL) {
#ifdef RECOMP_D3D_FRAME_ENABLED
        RecompD3dPresenter *presenter = recomp_d3d_frame_adapter_presenter();
        if (presenter != NULL) {
            RecompD3dPresenterError error = recomp_d3d_presenter_release_memory(
                presenter, allocation->base, allocation->size);
            recomp_d3d_frame_adapter_exit_if_closed(error);
            if (error != RECOMP_D3D_PRESENTER_OK) {
                recomp_stop(2, "d3d-release-memory:presenter:%u", (unsigned)error);
            }
        }
#endif
        allocation->active = 0;
        if (allocation->capacity != 0u) {
            coalesce_free_allocation(allocation);
        } else {
            /* Fixed-address virtual mappings do not own backing arena memory. */
            *allocation = (Allocation){0};
        }
    }
    xbox_HeapFree(base);
}

uint32_t recomp_kernel_allocate_pool(uint32_t size)
{
    return allocate_guest(size, 4u, PAGE_READWRITE, ALLOCATION_POOL);
}

void recomp_kernel_free_pool(uint32_t base)
{
    free_guest(base);
}

static void bridge_ex_allocate_pool(void)
{
    kernel_return(1u, recomp_kernel_allocate_pool(kernel_arg(1u)));
}

static void bridge_ex_allocate_pool_with_tag(void)
{
    kernel_return(
        2u,
        recomp_kernel_allocate_pool(kernel_arg(1u)));
}

static void bridge_ex_free_pool(void)
{
    free_guest(kernel_arg(1u));
    kernel_return(1u, 0u);
}

static void bridge_ex_query_pool_block_size(void)
{
    Allocation *allocation = find_allocation(kernel_arg(1u));
    uint32_t size = allocation != NULL && allocation->kind == ALLOCATION_POOL
        ? allocation->size
        : 0u;

    kernel_return(1u, size);
}

static void bridge_mm_allocate_contiguous_memory(void)
{
    kernel_return(
        1u,
        allocate_contiguous_guest(
            kernel_arg(1u), 0u, UINT32_MAX, 0u, PAGE_READWRITE));
}

static void bridge_mm_allocate_contiguous_memory_ex(void)
{
    uint32_t alignment = kernel_arg(4u);

    if (alignment < 4u) {
        alignment = 4u;
    }
    kernel_return(
        5u,
        allocate_contiguous_guest(
            kernel_arg(1u), kernel_arg(2u), kernel_arg(3u), alignment,
            kernel_arg(5u)));
}

static void bridge_mm_allocate_system_memory(void)
{
    kernel_return(
        2u,
        allocate_guest(
            kernel_arg(1u), 0x1000u, kernel_arg(2u), ALLOCATION_SYSTEM));
}

static void bridge_mm_claim_gpu_instance_memory(void)
{
    uint32_t padding_bytes = kernel_arg(2u);

    if (padding_bytes != 0u) {
        *recomp_memory_u32(padding_bytes) = 0x00010000u;
    }
    kernel_return(2u, 0x03ff0000u);
}

static void bridge_mm_create_kernel_stack(void)
{
    uint32_t size = kernel_arg(1u);
    uint32_t base;

    if (size == 0u) {
        size = 0x1000u;
    }
    base = allocate_guest(size, 0x1000u, PAGE_READWRITE, ALLOCATION_STACK);
    kernel_return(2u, base == 0u ? 0u : base + size);
}

static void bridge_mm_delete_kernel_stack(void)
{
    uint32_t stack_top = kernel_arg(1u);

    for (size_t i = 0; i < MAX_ALLOCATIONS; ++i) {
        if (allocations[i].active && allocations[i].kind == ALLOCATION_STACK &&
            (uint64_t)allocations[i].base + allocations[i].size == stack_top) {
            free_guest(allocations[i].base);
            break;
        }
    }
    kernel_return(2u, 0u);
}

static void bridge_mm_free_contiguous_memory(void)
{
    free_guest(kernel_arg(1u));
    kernel_return(1u, 0u);
}

static void bridge_mm_free_system_memory(void)
{
    free_guest(kernel_arg(1u));
    kernel_return(2u, 0u);
}

static void bridge_mm_get_physical_address(void)
{
    uint32_t address = kernel_arg(1u);

    if (address >= 0x80000000u && address < 0x84000000u) {
        address -= 0x80000000u;
    }
    kernel_return(1u, address);
}

static void bridge_mm_lock_unlock_buffer_pages(void)
{
    kernel_return(3u, 0u);
}

static void bridge_mm_lock_unlock_physical_page(void)
{
    kernel_return(2u, 0u);
}

static void bridge_mm_persist_contiguous_memory(void)
{
    kernel_return(3u, 0u);
}

static void bridge_mm_query_address_protect(void)
{
    Allocation *allocation = find_containing_allocation(kernel_arg(1u));

    kernel_return(
        1u, allocation == NULL ? PAGE_READWRITE : allocation->protect);
}

static void bridge_mm_query_allocation_size(void)
{
    Allocation *allocation = find_allocation(kernel_arg(1u));

    kernel_return(1u, allocation == NULL ? 0u : allocation->size);
}

static void bridge_mm_set_address_protect(void)
{
    Allocation *allocation = find_containing_allocation(kernel_arg(1u));

    if (allocation != NULL) {
        allocation->protect = kernel_arg(3u);
    }
    kernel_return(3u, 0u);
}

static void bridge_nt_allocate_virtual_memory(void)
{
    uint32_t base_pointer = kernel_arg(1u);
    uint32_t size_pointer = kernel_arg(3u);
    uint32_t base;
    uint32_t size;
    uint32_t protect = kernel_arg(5u);
    uint32_t status = STATUS_SUCCESS;

    if (base_pointer == 0u || size_pointer == 0u) {
        kernel_return(5u, STATUS_INVALID_PARAMETER);
        return;
    }

    base = *recomp_memory_u32(base_pointer);
    size = *recomp_memory_u32(size_pointer);
    if (size == 0u) {
        status = STATUS_INVALID_PARAMETER;
    } else if (base == 0u) {
        base = allocate_guest(
            size, 0x1000u, protect == 0u ? PAGE_READWRITE : protect,
            ALLOCATION_VIRTUAL);
        if (base == 0u) {
            status = STATUS_NO_MEMORY;
        }
    } else if (!guest_range_is_mapped(base, size) ||
               track_allocation(
                   base, size, protect == 0u ? PAGE_READWRITE : protect,
                   ALLOCATION_VIRTUAL) == NULL) {
        status = STATUS_NO_MEMORY;
    } else {
        recomp_guest_memset(base, 0, size);
    }

    if (status == STATUS_SUCCESS) {
        *recomp_memory_u32(base_pointer) = base;
    }
    kernel_return(5u, status);
}

static void bridge_nt_free_virtual_memory(void)
{
    uint32_t base_pointer = kernel_arg(1u);
    uint32_t size_pointer = kernel_arg(2u);
    uint32_t free_type = kernel_arg(3u);
    uint32_t base;
    Allocation *allocation;

    if (base_pointer == 0u) {
        kernel_return(3u, STATUS_INVALID_PARAMETER);
        return;
    }
    base = *recomp_memory_u32(base_pointer);
    allocation = find_allocation(base);
    if (base == 0u || allocation == NULL ||
        allocation->kind != ALLOCATION_VIRTUAL) {
        kernel_return(3u, STATUS_INVALID_PARAMETER);
        return;
    }

    if ((free_type & MEM_RELEASE) != 0u) {
        free_guest(base);
        *recomp_memory_u32(base_pointer) = 0u;
        if (size_pointer != 0u) {
            *recomp_memory_u32(size_pointer) = 0u;
        }
    }
    kernel_return(3u, STATUS_SUCCESS);
}

static void bridge_nt_query_virtual_memory(void)
{
    uint32_t return_length = kernel_arg(4u);

    if (return_length != 0u) {
        *recomp_memory_u32(return_length) = 0u;
    }
    kernel_return(4u, STATUS_NOT_IMPLEMENTED);
}

uint32_t recomp_kernel_load_section(uint32_t section)
{
    const uint32_t XBE_SECTION_HEADER_SIZE = 0x38u;
    const uint32_t XBE_SECTION_REFERENCE_COUNT = 0x18u;

    if (section == 0u || !guest_range_is_mapped(section, XBE_SECTION_HEADER_SIZE)) {
        return STATUS_INVALID_PARAMETER;
    }

    uint32_t *reference_count =
        recomp_memory_u32(section + XBE_SECTION_REFERENCE_COUNT);
    if (*reference_count == UINT32_MAX) {
        return STATUS_INVALID_PARAMETER;
    }
    ++*reference_count;
    return STATUS_SUCCESS;
}

uint32_t recomp_kernel_unload_section(uint32_t section)
{
    if (section == 0u || !guest_range_is_mapped(section, 0x38u)) {
        return STATUS_INVALID_PARAMETER;
    }
    uint32_t *reference_count = recomp_memory_u32(section + 0x18u);
    if (*reference_count == 0u) {
        return STATUS_INVALID_PARAMETER;
    }
    /* ponytail: the eager loader keeps section bytes resident; decommit needs
       matching reload support before either operation can release pages. */
    --*reference_count;
    return STATUS_SUCCESS;
}

static void bridge_xe_load_section(void)
{
    uint32_t section = kernel_arg(1u);
    uint32_t status = recomp_kernel_load_section(section);
    uint32_t reference_count = status == STATUS_SUCCESS
        ? *recomp_memory_u32(section + 0x18u)
        : 0u;

    fprintf(
        stderr,
        "recomp kernel: XeLoadSection section=0x%08x references=%u"
        " status=0x%08x\n",
        (unsigned)section,
        (unsigned)reference_count,
        (unsigned)status);
    kernel_return(1u, status);
}

static void bridge_xe_unload_section(void)
{
    uint32_t section = kernel_arg(1u);
    uint32_t status = recomp_kernel_unload_section(section);
    uint32_t reference_count = status == STATUS_SUCCESS
        ? *recomp_memory_u32(section + 0x18u)
        : 0u;

    fprintf(
        stderr,
        "recomp kernel: XeUnloadSection section=0x%08x references=%u"
        " status=0x%08x\n",
        (unsigned)section,
        (unsigned)reference_count,
        (unsigned)status);
    kernel_return(1u, status);
}

RecompFunction recomp_kernel_memory(uint32_t ordinal)
{
    switch (ordinal) {
    case 14u: return bridge_ex_allocate_pool;
    case 15u: return bridge_ex_allocate_pool_with_tag;
    case 17u: return bridge_ex_free_pool;
    case 23u: return bridge_ex_query_pool_block_size;
    case 165u: return bridge_mm_allocate_contiguous_memory;
    case 166u: return bridge_mm_allocate_contiguous_memory_ex;
    case 167u: return bridge_mm_allocate_system_memory;
    case 168u: return bridge_mm_claim_gpu_instance_memory;
    case 169u: return bridge_mm_create_kernel_stack;
    case 170u: return bridge_mm_delete_kernel_stack;
    case 171u: return bridge_mm_free_contiguous_memory;
    case 172u: return bridge_mm_free_system_memory;
    case 173u: return bridge_mm_get_physical_address;
    case 175u: return bridge_mm_lock_unlock_buffer_pages;
    case 176u: return bridge_mm_lock_unlock_physical_page;
    case 178u: return bridge_mm_persist_contiguous_memory;
    case 179u: return bridge_mm_query_address_protect;
    case 180u: return bridge_mm_query_allocation_size;
    case 182u: return bridge_mm_set_address_protect;
    case 184u: return bridge_nt_allocate_virtual_memory;
    case 199u: return bridge_nt_free_virtual_memory;
    case 217u: return bridge_nt_query_virtual_memory;
    case 327u: return bridge_xe_load_section;
    case 328u: return bridge_xe_unload_section;
    default: return NULL;
    }
}
