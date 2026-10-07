#ifndef DOAXBV_XBOX_MEMORY_LAYOUT_H
#define DOAXBV_XBOX_MEMORY_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/* Kernel data exports and synthetic thread objects. Above the main stack top
   (0x00f7fff0) and below the heap; the runner rejects an XBE image that
   reaches it. DOAXBV used 0x00740000, which is inside DOA3's .data BSS. */
#define XBOX_KERNEL_DATA_BASE 0x00f80000u
#define XBOX_STARTUP_THREAD_OBJECT (XBOX_KERNEL_DATA_BASE + 0x500u)
#define XBOX_STARTUP_THREAD_STACK_SLOT 0x03ffffc0u

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
void xbox_HeapFree(uint32_t guest_address);
uint32_t xbox_HeapCheckpoint(void);
bool xbox_HeapRestore(uint32_t checkpoint);
uint32_t xbox_ContiguousAlloc(
    uint32_t size,
    uint32_t lowest_address,
    uint32_t highest_address,
    uint32_t alignment);

#endif
