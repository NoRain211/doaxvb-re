#include "animation_probe.h"

#ifdef RECOMP_FULL_PROGRAM
#include "d3d_frame_adapter.h"
#include "stop_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sub_000AF050(void);
void sub_000AEEF0(void);
void sub_000AF5C0(void);
void sub_000636D0(void);
void sub_000B01E0(void);
void sub_00023B10(void);

/* Diagnostic records remain private. These snapshots are observations, not
   solver inputs or a substitute for reconstructing the palette independently. */
typedef struct PoseCapture {
    uint32_t version, frame, actor;
    uint32_t pose_address, offsets_address;
    uint8_t actor_before[0x180], actor_after[0x180];
    uint8_t root[0xbc];
    float pose_before[63], pose_after[63];
    float offsets[24][4], alternate_offsets[24][4];
    uint8_t limbs[4][24];
    uint8_t mode, paused, terrain_disabled, derived_enabled;
    float look_target[3];
    float bones_before[32][16], bones_after[32][16];
} PoseCapture;

static void note(unsigned entry)
{
    static uint32_t frame = UINT32_MAX;
    static uint32_t counts[6];
    uint32_t current = recomp_d3d_frame_adapter_swap_counter();
    if (current != frame) {
        if (frame != UINT32_MAX) {
            fprintf(stderr, "recomp animation dispatch: frame=%u calls=%u,%u,%u,%u,%u,%u\n",
                frame, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5]);
        }
        memset(counts, 0, sizeof counts);
        frame = current;
    }
    ++counts[entry];
}

static void integer_sample(void) { note(0); sub_000AF050(); }
static void fractional_sample(void) { note(1); sub_000AEEF0(); }
static void blend_pose(void) { note(2); sub_000AF5C0(); }
static void build_palette(void) { note(3); sub_000636D0(); }
static void build_camera(void) { note(5); sub_00023B10(); }

static void build_skeleton(void)
{
    uint32_t actor = recomp_runtime.registers.eax;
    uint32_t frame = recomp_d3d_frame_adapter_swap_counter();
    const char *prefix = getenv("RECOMP_POSE_CAPTURE");
    PoseCapture capture;
    uint32_t state = 0x005e5fe8u + actor * 0x180u;
    uint32_t bank = 0x004d3650u + actor * 0x800u;
    bool record = prefix != NULL && actor < 4u && frame >= 1900u && frame < 2020u;

    note(4);
    if (record) {
        memset(&capture, 0, sizeof capture);
        capture.version = 1u;
        capture.frame = frame;
        capture.actor = actor;
        capture.pose_address = *recomp_memory_u32(0x005e06a0u + actor * 0x5cu);
        capture.offsets_address = *recomp_memory_u32(state + 0x10u);
        recomp_guest_load(capture.actor_before, state, sizeof capture.actor_before);
        recomp_guest_load(capture.root, 0x004d6ff0u + actor * 0xbcu, sizeof capture.root);
        recomp_guest_load(capture.pose_before, capture.pose_address, sizeof capture.pose_before);
        recomp_guest_load(capture.offsets, capture.offsets_address, sizeof capture.offsets);
        recomp_guest_load(capture.alternate_offsets, *recomp_memory_u32(0x002d0c98u),
            sizeof capture.alternate_offsets);
        for (unsigned i = 0; i < 4u; ++i) {
            recomp_guest_load(capture.limbs[i], *recomp_memory_u32(state + 0x44u + i * 0x4cu),
                sizeof capture.limbs[i]);
        }
        capture.mode = *recomp_memory_i8(0x004d56dau);
        capture.paused = *recomp_memory_i8(0x004d56efu);
        capture.terrain_disabled = *recomp_memory_i8(0x004d96d8u + actor);
        capture.derived_enabled = *recomp_memory_i8(0x004cb648u + actor);
        recomp_guest_load(capture.look_target, 0x00416630u, sizeof capture.look_target);
        recomp_guest_load(capture.bones_before, bank, sizeof capture.bones_before);
    }
    sub_000B01E0();
    if (record) {
        char path[1024];
        recomp_guest_load(capture.actor_after, state, sizeof capture.actor_after);
        recomp_guest_load(capture.pose_after, capture.pose_address, sizeof capture.pose_after);
        recomp_guest_load(capture.bones_after, bank, sizeof capture.bones_after);
        int length = snprintf(path, sizeof path, "%s-%u-%u.bin", prefix, frame, actor);
        if (length < 0 || (size_t)length >= sizeof path) recomp_stop(1, "animation:capture-path");
        FILE *file = fopen(path, "wb");
        if (file == NULL) recomp_stop(1, "animation:capture-open");
        bool ok = fwrite(&capture, sizeof capture, 1, file) == 1;
        if (fclose(file) != 0 || !ok) recomp_stop(1, "animation:capture-write");
    }
}
#endif

RecompFunction recomp_animation_probe_lookup_manual(uint32_t address)
{
#ifdef RECOMP_FULL_PROGRAM
    switch (address) {
    case 0x000af050u: case 0x000aeef0u: case 0x000af5c0u:
    case 0x000636d0u: case 0x000b01e0u: case 0x00023b10u:
        if (getenv("RECOMP_ANIMATION_DISPATCH_TRACE") == NULL) return NULL;
        break;
    default: return NULL;
    }
    switch (address) {
    case 0x000af050u: return integer_sample;
    case 0x000aeef0u: return fractional_sample;
    case 0x000af5c0u: return blend_pose;
    case 0x000636d0u: return build_palette;
    case 0x000b01e0u: return build_skeleton;
    case 0x00023b10u: return build_camera;
    }
#else
    (void)address;
#endif
    return NULL;
}
