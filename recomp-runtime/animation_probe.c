#include "animation_probe.h"

#ifdef RECOMP_FULL_PROGRAM
#include "animation_skeleton.h"
#include "d3d_frame_adapter.h"
#include "stop_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
    RecompSkeletonTargets targets;
    RecompBoneMatrix solved[32];
} PoseCapture;

static RecompBoneMatrix solved_bones[4][32];
static uint32_t solved_frame[4];
static bool solved_valid[4];

static float guest_float(uint32_t address)
{
    float value;
    recomp_guest_load(&value, address, sizeof value);
    return value;
}

/* Read-only height lookup for the diagnostic input snapshot. The guest's
   bilinear grid uses low 12-bit elevations in units of 1/1024. */
static float ground_height(float x, float z)
{
    uint32_t grid = *recomp_memory_u32(0x009ef810u);
    if (grid == 0) return 0;
    float xmin = guest_float(0x009ef7e0u), zmin = guest_float(0x009ef7fcu);
    float base = guest_float(0x009ef7ecu), cell = guest_float(0x009ef7e8u);
    if (x < xmin || x > guest_float(0x009ef7e4u) ||
        z < zmin || z > guest_float(0x009ef808u)) return base;
    if (!isfinite(x) || !isfinite(z) || cell <= 0) recomp_stop(1, "animation:height-input");
    double inverse = 1.0/(double)cell;
    unsigned ix = (unsigned)(((double)x-xmin)*inverse);
    unsigned iz = (unsigned)(((double)z-zmin)*inverse);
    unsigned stride = *recomp_memory_u32(0x009ef800u);
    uint32_t at = grid + (ix*stride+iz)*2u;
    float fx = (float)(((double)x-((double)ix*cell+xmin))*inverse);
    float one_minus_x = 1.0f-fx;
    double fz = ((double)z-((double)iz*cell+zmin))*inverse;
    double h00 = *recomp_memory_u16(at) & 4095u;
    double h01 = *recomp_memory_u16(at+2u) & 4095u;
    double h10 = *recomp_memory_u16(at+stride*2u) & 4095u;
    double h11 = *recomp_memory_u16(at+stride*2u+2u) & 4095u;
    return (float)((((h11*fz*fx + h10*(1.0-fz)*fx) +
        fz*one_minus_x*h01) + (1.0-fz)*one_minus_x*h00)/1024.0 + base);
}

static float matrix_error(const RecompBoneMatrix *a, const RecompBoneMatrix *b, unsigned count)
{
    float maximum = 0;
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = 0; j < 16; ++j) {
            float error = fabsf(a[i].m[j]-b[i].m[j]);
            if (!isfinite(error)) return INFINITY;
            if (error > maximum) maximum = error;
        }
    return maximum;
}

static bool solve_capture(PoseCapture *c)
{
    RecompSkeletonTables t;
    RecompSkeletonTargets *s = &c->targets;
    float actor[96];
    memcpy(actor, c->actor_after, sizeof actor);
    memcpy(t.offsets, c->offsets, sizeof t.offsets);
    memcpy(t.alternate_offsets, c->alternate_offsets, sizeof t.alternate_offsets);
    memcpy(t.limbs, c->limbs, sizeof t.limbs);
    memset(s, 0, sizeof *s);
    memcpy(s->position, actor, sizeof s->position);
    s->heading = actor[3]; s->ground_height = actor[5];
    for (unsigned i = 0; i < 4; ++i) {
        const float *limb = actor+9u+i*19u;
        RecompLimbLengths *length = &s->lengths[i];
        length->proximal = limb[9]; length->distal = limb[10];
        length->squared_difference = limb[14];
        length->alternate_proximal = limb[11]; length->alternate_distal = limb[12];
        length->alternate_squared_difference = limb[16];
    }
    memcpy(s->target_offset, actor+6, sizeof s->target_offset);
    s->target_weight = actor[0x15c/4];
    s->upper_morph = c->actor_after[0x174]*(1.0f/30.0f);
    s->lower_morph = c->actor_after[0x175]*(1.0f/30.0f);
    memcpy(s->look_target, c->look_target, sizeof s->look_target);
    s->look_weight = actor[0x164/4];
    memcpy(s->eyes, c->pose_after+59, sizeof s->eyes);
    s->target_mask = c->actor_after[0x17f];
    s->minimum_bend = (c->actor_after[0x173]&2u) != 0;
    s->terrain_disabled = c->terrain_disabled;
    s->derived_blend = c->derived_enabled;
    /* The first solve supplies query positions before terrain correction. All
       matrices here are independently calculated, never copied from the oracle. */
    if (!recomp_animation_solve_skeleton(&t, c->pose_before+3, s, c->solved)) return false;
    const float *hip = c->solved[2].m, *body = c->solved[0].m, *root = c->solved[15].m;
    s->hip_height = ground_height(hip[12], hip[14]);
    float neck[3];
    for (unsigned i = 0; i < 3; ++i)
        neck[i] = (float)((double)t.offsets[19][0]*body[i] +
            (double)t.offsets[19][1]*body[4+i] + (double)t.offsets[19][2]*body[8+i] + body[12+i]);
    s->neck_height = ground_height(neck[0], neck[2]);
    for (unsigned limb = 0; limb < 4; ++limb) {
        float point[3];
        for (unsigned i = 0; i < 3; ++i)
            point[i] = (float)((double)c->pose_before[3+t.limbs[limb].position_channels[0]]*root[i] +
                (double)c->pose_before[3+t.limbs[limb].position_channels[1]]*root[4+i] +
                (double)c->pose_before[3+t.limbs[limb].position_channels[2]]*root[8+i] + root[12+i]);
        s->limb_height[limb] = ground_height(point[0], point[2]);
    }
    return recomp_animation_solve_skeleton(&t, c->pose_before+3, s, c->solved);
}

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
static void build_palette(void)
{
    uint32_t frame = recomp_d3d_frame_adapter_swap_counter();
    uint32_t bank = *recomp_memory_u32(recomp_runtime.registers.esp+4u);
    uint32_t offset_address = recomp_runtime.registers.edi;
    uint32_t recipe_id = recomp_runtime.registers.eax;
    bool verify = getenv("RECOMP_SKELETON_VERIFY") != NULL && frame >= 1900u && frame < 2020u;
    uint8_t recipe[16];
    unsigned output_count = 0;
    float offsets[24][4];
    RecompBoneMatrix bones[32], initial, expected[4], scratch;
    unsigned actor = (bank-0x004d3650u)/0x800u;
    bool actor_bank = bank >= 0x004d3650u && actor < 4u && (bank-0x004d3650u)%0x800u == 0;
    note(3);
    if (verify) {
        uint32_t objects = *recomp_memory_u32(*recomp_ebp_register()-4u);
        unsigned ordinal = *(const uint8_t *)recomp_memory_i8(recomp_runtime.registers.esi);
        uint32_t object = *recomp_memory_u32(objects+ordinal*4u);
        output_count = *recomp_memory_u32(object+4u)+1u;
        if (output_count == 0 || output_count > 4u) recomp_stop(1, "animation:palette-influences");
        recomp_guest_load(recipe, 0x0033f988u+recipe_id*16u, sizeof recipe);
        recomp_guest_load(bones, bank, sizeof bones);
        recomp_guest_load(offsets, offset_address, sizeof offsets);
        recomp_guest_load(&initial, 0x00a24190u, sizeof initial);
        if (!recomp_animation_build_palette(recipe, sizeof recipe, output_count, bones, 32, offsets, 24,
            &initial, expected, &scratch)) {
            fprintf(stderr, "recomp animation palette rejected: frame=%u recipe=%u declared=%u used=%u\n",
                frame, recipe_id, recipe[0], output_count);
            recomp_stop(1, "animation:palette-recipe");
        }
    }
    sub_000636D0();
    if (verify) {
        RecompBoneMatrix actual[4], native[4];
        recomp_guest_load(actual, 0x00b25840u, output_count*sizeof *actual);
        float recipe_error = matrix_error(expected, actual, output_count);
        float pose_error = -1;
        if (actor_bank && solved_valid[actor] && solved_frame[actor] == frame) {
            if (!recomp_animation_build_palette(recipe, sizeof recipe, output_count, solved_bones[actor], 32,
                offsets, 24, &initial, native, &scratch)) recomp_stop(1, "animation:solved-palette-recipe");
            pose_error = matrix_error(native, actual, output_count);
        }
        fprintf(stderr, "recomp animation palette: frame=%u recipe=%u count=%u actor=%u recipe_error=%.9g pose_error=%.9g\n",
            frame, recipe_id, output_count, actor_bank ? actor : UINT32_MAX, recipe_error, pose_error);
        if (recipe_error > 1e-5f || pose_error > 1e-4f) recomp_stop(1, "animation:palette-mismatch");
    }
}
static void build_camera(void) { note(5); sub_00023B10(); }

static void build_skeleton(void)
{
    uint32_t actor = recomp_runtime.registers.eax;
    uint32_t frame = recomp_d3d_frame_adapter_swap_counter();
    const char *prefix = getenv("RECOMP_POSE_CAPTURE");
    PoseCapture capture;
    uint32_t state = 0x005e5fe8u + actor * 0x180u;
    uint32_t bank = 0x004d3650u + actor * 0x800u;
    bool verify = getenv("RECOMP_SKELETON_VERIFY") != NULL;
    bool record = (prefix != NULL || verify) && actor < 4u && frame >= 1900u && frame < 2020u;

    note(4);
    if (record) {
        memset(&capture, 0, sizeof capture);
        capture.version = 2u;
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
        bool solved = solve_capture(&capture);
        float error = solved ? matrix_error(capture.solved,
            (const RecompBoneMatrix *)(const void *)capture.bones_after, 32) : INFINITY;
        fprintf(stderr, "recomp animation skeleton: frame=%u actor=%u solved=%u error=%.9g look=%.9g morph=%.9g,%.9g terrain=%.9g,%.9g\n",
            frame, actor, solved, error, capture.targets.look_weight, capture.targets.upper_morph,
            capture.targets.lower_morph, capture.targets.hip_height, capture.targets.neck_height);
        solved_valid[actor] = solved;
        solved_frame[actor] = frame;
        if (solved) memcpy(solved_bones[actor], capture.solved, sizeof capture.solved);
        if (prefix != NULL) {
            int length = snprintf(path, sizeof path, "%s-%u-%u.bin", prefix, frame, actor);
            if (length < 0 || (size_t)length >= sizeof path) recomp_stop(1, "animation:capture-path");
            FILE *file = fopen(path, "wb");
            if (file == NULL) recomp_stop(1, "animation:capture-open");
            bool ok = fwrite(&capture, sizeof capture, 1, file) == 1;
            if (fclose(file) != 0 || !ok) recomp_stop(1, "animation:capture-write");
        }
        if (verify && error > 1e-4f) recomp_stop(1, "animation:skeleton-mismatch");
    }
}
#endif

RecompFunction recomp_animation_probe_lookup_manual(uint32_t address)
{
#ifdef RECOMP_FULL_PROGRAM
    switch (address) {
    case 0x000af050u: case 0x000aeef0u: case 0x000af5c0u:
    case 0x000636d0u: case 0x000b01e0u: case 0x00023b10u:
        if (getenv("RECOMP_ANIMATION_DISPATCH_TRACE") == NULL &&
            getenv("RECOMP_SKELETON_VERIFY") == NULL) return NULL;
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
