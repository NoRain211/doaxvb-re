#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "animation_track_adapter.h"
#include "program_manual.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int recomp_animation_track_adapter_test(void)
{
    uint8_t memory[256] = {0};
    uint8_t expected[256];
    RecompMemoryRegion region = {0x20000000u, sizeof memory, memory};
    const uint32_t addresses[] = {0xaea60u, 0xaeb50u, 0xaebc0u, 0xaec50u};
    const float coefficients[][4] = {
        {1.0f, 0.0f, 0.375f, -0.0625f},
        {1.0f, 0.5f, 0.0f, 0.0f},
        {1.0f, 0.5f, -0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 0.0f}
    };
    const char *setting = getenv("RECOMP_ANIMATION_TRACKS");
    char *original = setting ? malloc(strlen(setting)+1) : NULL;
    if (setting && !original) return 0;
    if (original) strcpy(original, setting);
    int passed = 1;
    const unsigned endpoint[] = {10u, 4u, 6u, 0u};
#ifdef _WIN32
    _putenv_s("RECOMP_ANIMATION_TRACKS", "1");
#else
    setenv("RECOMP_ANIMATION_TRACKS", "1", 1);
#endif
    for (unsigned form = 0; form < 4u; ++form) {
        RecompRegisters saved = {1u, region.address, region.address + 64u,
            4u, 5u, 6u, 7u, region.address + 240u};
        RecompFpuContext fpu, after;
        RecompFunction entry;
        memset(memory, 0, sizeof memory);
        memory[12] = 4u;
        memory[64 + 1] = 0x10u; /* synthetic value 1 */
        if (form != 3u) {
            memory[64 + endpoint[form] + 1] = 0x20u;
            memory[64 + endpoint[form] + 3] = 0x80u; /* value 3 */
        }
        memcpy(expected, memory, sizeof memory);
        memcpy(expected + 16, coefficients[form], sizeof coefficients[form]);
        recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
        recomp_runtime.registers = saved;
        recomp_fpu_context_save(&fpu);
        entry = recomp_lookup_manual(addresses[form]);
        if (entry == NULL) {
            fprintf(stderr, "Animation adapter: form %u address 0x%08x lookup returned NULL\n", form, addresses[form]);
            passed = 0;
            break;
        }
        entry();
        recomp_fpu_context_save(&after);
        saved.esp += 4u;
        if (memcmp(memory, expected, sizeof memory) != 0 ||
            memcmp(&saved, &recomp_runtime.registers, sizeof saved) != 0 ||
            memcmp(fpu.fpu_stack, after.fpu_stack, sizeof fpu.fpu_stack) != 0 ||
            fpu.fpu_top != after.fpu_top) {
            fprintf(stderr, "Animation adapter: form %u changed unexpected state\n", form);
            passed = 0;
            break;
        }
    }
#ifdef _WIN32
    _putenv_s("RECOMP_ANIMATION_TRACKS", original ? original : "");
#else
    if (original) setenv("RECOMP_ANIMATION_TRACKS", original, 1);
    else unsetenv("RECOMP_ANIMATION_TRACKS");
#endif
    free(original);
    return passed && recomp_animation_track_lookup_manual(0u) == NULL;
}
