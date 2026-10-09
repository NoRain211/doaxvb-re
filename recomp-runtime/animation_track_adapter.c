#include "animation_track_adapter.h"
#include "animation_track.h"
#include "animation_probe.h"
#include "stop_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RECOMP_FULL_PROGRAM
#include "d3d_frame_adapter.h"
void sub_000AEA60(void);
void sub_000AEB50(void);
void sub_000AEBC0(void);
void sub_000AEC50(void);
#endif

static int mode(void)
{
    static int selected = -1;
    if (selected < 0) {
        const char *value = getenv("RECOMP_ANIMATION_TRACKS");
        selected = value != NULL && strcmp(value, "1") == 0 ? 1 :
            value != NULL && strcmp(value, "verify") == 0 ? 2 : 0;
    }
    return selected;
}

static void build_curve(unsigned form)
{
    static const size_t required[] = {14u, 8u, 10u, 4u};
    uint32_t cursor = recomp_runtime.registers.ecx;
    uint32_t record = recomp_runtime.registers.edx;
    uint32_t duration = *recomp_memory_u32(cursor + 12u);
    RecompAnimationCurve curve;

#ifdef RECOMP_FULL_PROGRAM
    static const RecompFunction original[] = {
        sub_000AEA60, sub_000AEB50, sub_000AEBC0, sub_000AEC50
    };
#endif
    if (!recomp_animation_curve(form, recomp_memory(record, required[form]),
            required[form], duration, &curve)) {
#ifdef RECOMP_FULL_PROGRAM
        if (mode() == 1) {
            original[form]();
            return;
        }
#endif
        recomp_stop(1, "animation:invalid-segment");
        return;
    }
#ifdef RECOMP_FULL_PROGRAM
    if (mode() == 2) {
        static uint32_t previous_frame = UINT32_MAX;
        static uint32_t compared[4];
        uint32_t frame = recomp_d3d_frame_adapter_swap_counter();

        /* Diagnostic oracle only: execute the original once, preserving its
           volatile registers, stack scratch and FPU side effects. The native
           candidate reads immutable inputs and never writes guest memory. */
        original[form]();
        if (memcmp(recomp_memory(cursor + 16u, sizeof curve), &curve, sizeof curve) != 0) {
            fprintf(stderr, "recomp animation: coefficient mismatch frame=%u form=%u\n",
                frame, form);
            recomp_stop(1, "animation:coefficient-mismatch");
            return;
        }
        if (frame != previous_frame) {
            if (previous_frame != UINT32_MAX) {
                fprintf(stderr, "recomp animation: frame=%u identical=%u,%u,%u,%u\n",
                    previous_frame, compared[0], compared[1], compared[2], compared[3]);
            }
            memset(compared, 0, sizeof compared);
            previous_frame = frame;
        }
        ++compared[form];
        return;
    }
#endif
    recomp_guest_store(cursor + 16u, &curve, sizeof curve);
    recomp_runtime.registers.esp += 4u;
}

static void hermite(void) { build_curve(0u); }
static void linear(void) { build_curve(1u); }
static void quadratic(void) { build_curve(2u); }
static void constant(void) { build_curve(3u); }

RecompFunction recomp_animation_track_lookup_manual(uint32_t guest_address)
{
    /* Game-owned coefficient builders called indirectly by the track walker.
       Model rejection in mode 1 calls the generated builder; verify mode uses
       it as an oracle. Direct walker/sampler calls bypass manual lookup. */
    switch (guest_address) {
    case 0x000aea60u: return mode() ? hermite : NULL;
    case 0x000aeb50u: return mode() ? linear : NULL;
    case 0x000aebc0u: return mode() ? quadratic : NULL;
    case 0x000aec50u: return mode() ? constant : NULL;
    default: return recomp_animation_probe_lookup_manual(guest_address);
    }
}
