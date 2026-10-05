#include "animation_probe.h"
#include "animation_split_adapter.h"

#ifdef RECOMP_FULL_PROGRAM
#include "animation_skeleton.h"
#include "animation_pose.h"
#include "animation_seam.h"
#include "d3d_pose_replay.h"
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
void sub_0017D520(void);
void sub_0017D670(void);

/* Diagnostic records remain private. Oracle bone outputs enter comparisons
   only; immutable rig, channel and controller snapshots feed the plain solver. */
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
static PoseCapture split_captures[4];
static uint32_t solved_frame[4];
static bool solved_valid[4];
static PoseCapture experiment_previous;
static bool experiment_previous_valid, experiment_ready;
static RecompBoneMatrix experiment_bones[RECOMP_POSE_REPLAY_SAMPLES][32];
typedef struct PoseBinding {
    uint32_t object;
    RecompBoneMatrix original[4];
    RecompD3dPoseReplay replay;
} PoseBinding;
static PoseBinding experiment_binding, experiment_bindings[128];
static unsigned experiment_binding_count;

static void remember_binding(void)
{
    if (experiment_binding_count == sizeof experiment_bindings/sizeof *experiment_bindings)
        recomp_stop(1, "animation:experiment-bindings-full");
    experiment_bindings[experiment_binding_count++] = experiment_binding;
}

static uint32_t experiment_frame(void)
{
    const char *text = getenv("RECOMP_POSE_EXPERIMENT_FRAME");
    if (text == NULL) return 0;
    char *end;
    unsigned long frame = strtoul(text, &end, 10);
    return *text != '\0' && *end == '\0' && frame > 0 && frame < UINT32_MAX ? (uint32_t)frame : 0;
}

static unsigned experiment_actor(void)
{
    const char *text = getenv("RECOMP_POSE_EXPERIMENT_ACTOR");
    return text == NULL ? 0u : (unsigned)strtoul(text, NULL, 10);
}

static uint32_t state_capture_frame(void)
{
    const char *text = getenv("RECOMP_POSE_STATE_CAPTURE_AT");
    if (!text) return 0;
    char *end;
    unsigned long frame = strtoul(text, &end, 10);
    return *text && !*end && frame < UINT32_MAX ? (uint32_t)frame : 0;
}

static uint32_t seam_record(uint32_t object, unsigned actor)
{
    if (actor >= 4) return 0;
    unsigned count = *(const uint8_t *)recomp_memory_i8(0x004cb644u+actor);
    unsigned mask = *(const uint8_t *)recomp_memory_i8(0x004cb640u+actor);
    if (count > 32) recomp_stop(1, "animation:seam-record-count");
    for (unsigned i = 0; i < count; ++i) {
        uint32_t record = 0x004ca640u+actor*0x400u+i*32u;
        if (*recomp_memory_u32(record) == object &&
            (*(const uint8_t *)recomp_memory_i8(record+14u)&mask) == 0) return record;
    }
    return 0;
}

/* 0x58A50 binds these attachment records through a separate SDK callback.
   Resolve the actor-owned descriptor rather than guessing from matrix values. */
static unsigned attachment_bone(uint32_t object, unsigned actor)
{
    if (actor >= 4) return UINT32_MAX;
    uint32_t owner = 0x004256f8u+actor*0x1b668u;
    unsigned count = *(const uint8_t *)recomp_memory_i8(owner+6);
    for (unsigned i = 0; i < count; ++i) {
        uint32_t entry = owner+0x3474u+i*8u;
        unsigned kind = *(const uint8_t *)recomp_memory_i8(entry);
        unsigned flags = *(const uint8_t *)recomp_memory_i8(entry+1);
        if (!kind || kind > 3 || (flags&2)) continue;
        uint32_t descriptor = *recomp_memory_u32(entry+4);
        unsigned mesh = *recomp_memory_u16(descriptor);
        unsigned bone = *(const uint8_t *)recomp_memory_i8(descriptor+3);
        if (mesh >= 128 || bone >= 32) continue;
        if (*recomp_memory_u32(0x00b34a80u+actor*0x200u+mesh*4u) == object) return bone;
    }
    return UINT32_MAX;
}

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

static void evaluate_experiment_pose(const PoseCapture *previous, const PoseCapture *current,
    float fraction, RecompBoneMatrix output[32])
{
    RecompAnimationPose a, b, blended;
    RecompAnimationGroup groups[24];
    memcpy(&a, previous->pose_before, sizeof a);
    memcpy(&b, current->pose_before, sizeof b);
    blended = a;
    recomp_guest_load(groups, 0x002cfb70u, sizeof groups);
    if (!recomp_animation_pose_blend(groups, 24, &a, &b, fraction, &blended))
        recomp_stop(1, "animation:experiment-blend");
    PoseCapture sample = *previous;
    memcpy(sample.pose_before, &blended, sizeof blended);
    float old_actor[96], new_actor[96];
    memcpy(old_actor, sample.actor_after, sizeof old_actor);
    memcpy(new_actor, current->actor_after, sizeof new_actor);
    for (unsigned i = 0; i < 3; ++i) old_actor[i] = (float)(old_actor[i]+((double)new_actor[i]-old_actor[i])*fraction);
    const double pi = 3.14159265358979323846;
    double angle = fmod((double)new_actor[3]-old_actor[3]+pi, 2*pi);
    if (angle < 0) angle += 2*pi;
    old_actor[3] = (float)(old_actor[3]+(angle-pi)*fraction);
    memcpy(sample.actor_after, old_actor, sizeof old_actor);
    if (!solve_capture(&sample)) recomp_stop(1, "animation:experiment-solve");
    memcpy(output, sample.solved, sizeof sample.solved);
}

static void prepare_experiment(const PoseCapture *current)
{
    uint32_t frame = experiment_frame();
    if (frame == 0 || current->actor != experiment_actor()) return;
    if (current->frame+1 == frame) {
        experiment_previous = *current;
        experiment_previous_valid = true;
        return;
    }
    if (current->frame != frame || !experiment_previous_valid ||
        experiment_previous.frame+1 != frame) return;
    if (memcmp(current->offsets, experiment_previous.offsets, sizeof current->offsets) != 0 ||
        memcmp(current->limbs, experiment_previous.limbs, sizeof current->limbs) != 0)
        recomp_stop(1, "animation:experiment-rig-change");
    void *guest_before = malloc(RECOMP_XBOX_RAM_SIZE);
    if (guest_before == NULL) recomp_stop(1, "animation:experiment-snapshot");
    memcpy(guest_before, recomp_memory(0, RECOMP_XBOX_RAM_SIZE), RECOMP_XBOX_RAM_SIZE);
    memcpy(experiment_bones[0], experiment_previous.solved, sizeof current->solved);
    for (unsigned phase = 1; phase+1 < RECOMP_POSE_REPLAY_SAMPLES; ++phase) {
        float fraction = (float)phase/(RECOMP_POSE_REPLAY_SAMPLES-1);
        evaluate_experiment_pose(&experiment_previous, current, fraction, experiment_bones[phase]);
    }
    memcpy(experiment_bones[RECOMP_POSE_REPLAY_SAMPLES-1], current->solved, sizeof current->solved);
    bool unchanged = memcmp(guest_before, recomp_memory(0, RECOMP_XBOX_RAM_SIZE), RECOMP_XBOX_RAM_SIZE) == 0;
    free(guest_before);
    if (!unchanged) recomp_stop(1, "animation:experiment-guest-write");
    experiment_ready = true;
    experiment_binding_count = 0;
    fprintf(stderr, "recomp pose experiment prepared: frame=%u actor=%u guest_bytes_unchanged=%u first_sample_delta=%.9g\n",
        frame, current->actor, RECOMP_XBOX_RAM_SIZE,
        matrix_error(experiment_bones[0], experiment_bones[1], 32));
}

static void note(unsigned entry)
{
    if (recomp_animation_split_enabled() && !experiment_frame() && !getenv("RECOMP_ANIMATION_DISPATCH_TRACE")) return;
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
    uint32_t object = 0;
    float offsets[24][4];
    RecompBoneMatrix bones[32], initial, expected[4], scratch;
    unsigned actor = (bank-0x004d3650u)/0x800u;
    bool actor_bank = bank >= 0x004d3650u && actor < 4u && (bank-0x004d3650u)%0x800u == 0;
    bool experiment = frame == experiment_frame() && experiment_ready &&
        actor_bank && actor == experiment_actor();
    bool split = recomp_animation_split_enabled() && actor_bank;
    note(3);
    if (verify || experiment || split) {
        uint32_t objects = *recomp_memory_u32(*recomp_ebp_register()-4u);
        unsigned ordinal = *(const uint8_t *)recomp_memory_i8(recomp_runtime.registers.esi);
        object = *recomp_memory_u32(objects+ordinal*4u);
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
    if (verify || experiment || split) {
        RecompBoneMatrix actual[4], native[4];
        recomp_guest_load(actual, 0x00b25840u, output_count*sizeof *actual);
        float recipe_error = matrix_error(expected, actual, output_count);
        float pose_error = -1;
        if (actor_bank && solved_valid[actor] && solved_frame[actor] == frame) {
            if (!recomp_animation_build_palette(recipe, sizeof recipe, output_count, solved_bones[actor], 32,
                offsets, 24, &initial, native, &scratch)) recomp_stop(1, "animation:solved-palette-recipe");
            pose_error = matrix_error(native, actual, output_count);
        }
        if (!split || verify) fprintf(stderr, "recomp animation palette: frame=%u recipe=%u count=%u actor=%u recipe_error=%.9g pose_error=%.9g\n",
            frame, recipe_id, output_count, actor_bank ? actor : UINT32_MAX, recipe_error, pose_error);
        if (recipe_error > 1e-5f || pose_error > 1e-4f) {
            fprintf(stderr, "recomp animation palette mismatch: frame=%u actor=%u recipe=%u recipe_error=%.9g pose_error=%.9g\n",
                frame, actor, recipe_id, recipe_error, pose_error);
            const char *failure = getenv("RECOMP_SPLIT_FAILURE");
            if (failure && actor < 4) {
                FILE *file = fopen(failure, "wb");
                if (file) {
                    fwrite(split_captures+actor, sizeof split_captures[0], 1, file);
                    RecompBoneMatrix current_bones[32];
                    recomp_guest_load(current_bones, bank, sizeof current_bones);
                    fwrite(current_bones, sizeof current_bones, 1, file);
                    fwrite(recipe, sizeof recipe, 1, file);
                    fwrite(offsets, sizeof offsets, 1, file);
                    fwrite(&initial, sizeof initial, 1, file);
                    fwrite(actual, output_count*sizeof *actual, 1, file);
                    fclose(file);
                }
            }
            recomp_stop(1, "animation:palette-mismatch");
        }
        if (split) recomp_animation_split_palette(actor, frame, object, output_count, recipe, offsets, &initial, actual);
        if (experiment) {
            RecompD3dPoseReplay *replay = &experiment_binding.replay;
            memset(replay, 0, sizeof *replay);
            replay->frame = frame; replay->actor = actor;
            replay->recipe = recipe_id; replay->count = output_count;
            for (unsigned phase = 0; phase < RECOMP_POSE_REPLAY_SAMPLES; ++phase) {
                if (!recomp_animation_build_palette(recipe, sizeof recipe, output_count,
                    experiment_bones[phase], 32, offsets, 24, &initial, native, &scratch))
                    recomp_stop(1, "animation:experiment-palette");
                memcpy(replay->palettes[phase], native, output_count*sizeof *native);
            }
            experiment_binding.object = object;
            memcpy(experiment_binding.original, actual, output_count*sizeof *actual);
            remember_binding();
        }
    }
}
static void build_camera(void)
{
    uint32_t address = recomp_runtime.registers.eax;
    note(5); sub_00023B10();
    recomp_animation_split_camera(address);
}

static void bind_rigid_pose(void)
{
    recomp_animation_split_rigid();
    if (!experiment_ready || recomp_d3d_frame_adapter_swap_counter() != experiment_frame()) return;
    uint32_t context = recomp_runtime.registers.ebx;
    uint32_t caller = *recomp_ebp_register();
    if (context > RECOMP_XBOX_RAM_SIZE-0x2cu || caller < 8u || caller >= RECOMP_XBOX_RAM_SIZE) return;
    uint32_t bank = *recomp_memory_u32(context+0x1cu);
    uint32_t bone = *recomp_memory_u32(caller-8u);
    if (bank != 0x004d3650u+experiment_actor()*0x800u ||
        bone < bank || bone >= bank+0x800u || (bone-bank)%64u != 0) return;
    RecompBoneMatrix original;
    recomp_guest_load(&original, bone, sizeof original);
    if (memcmp(&original, recomp_memory(0x00a24190u, sizeof original), sizeof original) != 0) return;
    unsigned joint = (bone-bank)/64u;
    RecompD3dPoseReplay *replay = &experiment_binding.replay;
    memset(replay, 0, sizeof *replay);
    replay->frame = experiment_frame(); replay->actor = experiment_actor();
    replay->recipe = UINT32_MAX; replay->count = 1;
    for (unsigned phase = 0; phase < RECOMP_POSE_REPLAY_SAMPLES; ++phase)
        memcpy(replay->palettes[phase][0], experiment_bones[phase][joint].m, sizeof original);
    experiment_binding.object = recomp_runtime.registers.ecx;
    experiment_binding.original[0] = original;
    remember_binding();
    fprintf(stderr, "recomp pose rigid binding: frame=%u actor=%u joint=%u object=%08x\n",
        replay->frame, replay->actor, joint, experiment_binding.object);
}

static void draw_rigid(void) { bind_rigid_pose(); sub_0017D520(); }
static void draw_object(void)
{
    if ((recomp_runtime.registers.edx&1u) == 0) bind_rigid_pose();
    sub_0017D670();
}

static void build_skeleton(void)
{
    uint32_t actor = recomp_runtime.registers.eax;
    uint32_t frame = recomp_d3d_frame_adapter_swap_counter();
    const char *prefix = getenv("RECOMP_POSE_CAPTURE");
    PoseCapture capture;
    uint32_t state = 0x005e5fe8u + actor * 0x180u;
    uint32_t bank = 0x004d3650u + actor * 0x800u;
    bool verify = getenv("RECOMP_SKELETON_VERIFY") != NULL;
    uint32_t experiment = experiment_frame();
    bool experiment_capture = experiment > 0 && actor == experiment_actor() &&
        (frame == experiment || frame+1 == experiment);
    uint32_t state_frame = state_capture_frame();
    bool record = actor < 4u && (recomp_animation_split_enabled() || experiment_capture ||
        (state_frame && (frame == state_frame || frame+1 == state_frame)) ||
        ((prefix != NULL || verify) && frame >= 1900u && frame < 2020u));

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
        if (!recomp_animation_split_enabled() || verify) fprintf(stderr, "recomp animation skeleton: frame=%u actor=%u solved=%u error=%.9g look=%.9g morph=%.9g,%.9g terrain=%.9g,%.9g\n",
            frame, actor, solved, error, capture.targets.look_weight, capture.targets.upper_morph,
            capture.targets.lower_morph, capture.targets.hip_height, capture.targets.neck_height);
        solved_valid[actor] = solved;
        solved_frame[actor] = frame;
        if (solved) memcpy(solved_bones[actor], capture.solved, sizeof capture.solved);
        if (solved) prepare_experiment(&capture);
        if (solved && recomp_animation_split_enabled()) {
            split_captures[actor] = capture;
            RecompVisualPose visual = {0};
            memcpy(visual.tables.offsets, capture.offsets, sizeof capture.offsets);
            memcpy(visual.tables.alternate_offsets, capture.alternate_offsets, sizeof capture.alternate_offsets);
            memcpy(visual.tables.limbs, capture.limbs, sizeof capture.limbs);
            recomp_guest_load(visual.groups, 0x002cfb70u, sizeof visual.groups);
            memcpy(visual.channels, capture.pose_before, sizeof visual.channels[0]);
            visual.targets[0] = capture.targets;
            memcpy(visual.endpoints[0], capture.solved, sizeof capture.solved);
            recomp_animation_split_capture(actor, frame, &visual);
        }

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

void recomp_animation_probe_capture_frame(uint32_t frame)
{
#ifdef RECOMP_FULL_PROGRAM
    uint32_t target = state_capture_frame();
    const char *prefix = getenv("RECOMP_POSE_STATE_CAPTURE");
    if (!target || !prefix || (frame != target && frame+1 != target)) return;
    char path[1024];
    int length = snprintf(path, sizeof path, "%s-%u.bin", prefix, frame);
    if (length < 0 || (size_t)length >= sizeof path) recomp_stop(1, "animation:state-path");
    FILE *file = fopen(path, "wb");
    if (!file) recomp_stop(1, "animation:state-open");
    bool ok = fwrite(recomp_memory(0, RECOMP_XBOX_RAM_SIZE), RECOMP_XBOX_RAM_SIZE, 1, file) == 1;
    if (fclose(file) != 0 || !ok) recomp_stop(1, "animation:state-write");
    fprintf(stderr, "recomp animation state capture: frame=%u bytes=%u\n", frame, RECOMP_XBOX_RAM_SIZE);
#else
    (void)frame;
#endif
}

void recomp_animation_probe_capture_vertices(const struct RecompD3dPresenterDrawCommand *draw,
    const float worlds[4][16], unsigned count)
{
#ifdef RECOMP_FULL_PROGRAM
    uint32_t frame = recomp_d3d_frame_adapter_swap_counter();
    const char *prefix = getenv("RECOMP_POSE_VERTEX_CAPTURE");
    uint32_t target = state_capture_frame() ? state_capture_frame() : experiment_frame();
    if (!prefix || !target || (frame != target && frame+1 != target)) return;
    char path[1024];
    int length = snprintf(path, sizeof path, "%s-%u.bin", prefix, frame);
    if (length < 0 || length >= sizeof path) recomp_stop(1, "animation:vertex-path");
    static uint32_t last_frame = UINT32_MAX;
    FILE *file = fopen(path, frame == last_frame ? "ab" : "wb");
    if (!file) recomp_stop(1, "animation:vertex-open");
    bool first = frame != last_frame;
    last_frame = frame;
    uint32_t header[8] = {frame, *recomp_memory_u32(0x00a2479cu), *recomp_memory_u32(0x00a247a8u),
        draw->fvf, draw->vertex_stride, draw->vertex_count, draw->index_count, count};
    bool ok = fwrite(header, sizeof header, 1, file) == 1 &&
        fwrite(worlds, count*64u, 1, file) == 1 &&
        fwrite(draw->vertex_bytes, draw->vertex_stride*draw->vertex_count, 1, file) == 1 &&
        fwrite(draw->index_bytes, draw->index_count*2u, 1, file) == 1;
    if (fclose(file) != 0 || !ok) recomp_stop(1, "animation:vertex-write");
    length = snprintf(path, sizeof path, "%s-%u.jsonl", prefix, frame);
    if (length < 0 || (size_t)length >= sizeof path) recomp_stop(1, "animation:vertex-map-path");
    file = fopen(path, first ? "wb" : "ab");
    if (!file) recomp_stop(1, "animation:vertex-map-open");
    uintptr_t vertex = (uintptr_t)draw->vertex_bytes;
    uintptr_t base = (uintptr_t)recomp_memory(0, RECOMP_XBOX_RAM_SIZE);
    uint32_t address = vertex >= base && vertex-base < RECOMP_XBOX_RAM_SIZE ? (uint32_t)(vertex-base) : UINT32_MAX;
    fprintf(file, "{\"object\":%u,\"vertex_address\":%u,\"vertices\":%u,\"stride\":%u}\n",
        header[1], address, draw->vertex_count, draw->vertex_stride);
    if (fclose(file) != 0) recomp_stop(1, "animation:vertex-map-write");
#else
    (void)draw; (void)worlds; (void)count;
#endif
}

bool recomp_animation_probe_pose_replay(const float worlds[4][16], unsigned count,
    struct RecompD3dPoseReplay *output)
{
#ifdef RECOMP_FULL_PROGRAM
    if (!output || !experiment_ready || count > 4 ||
        recomp_d3d_frame_adapter_swap_counter() != experiment_frame()) return false;
    const PoseBinding *found = NULL;
    uint32_t object = *recomp_memory_u32(0x00a2479cu);
    /* The SDK queues copies of palettes for later passes. The copied address
       is not provenance; object and exact original palette survive the queue. */
    for (unsigned i = 0; i < experiment_binding_count; ++i) {
        const PoseBinding *binding = experiment_bindings+i;
        if (object != binding->object || count != binding->replay.count ||
            memcmp(worlds, binding->original, count*sizeof *worlds) != 0) continue;
        if (found && memcmp(found->replay.palettes, binding->replay.palettes,
            sizeof binding->replay.palettes) != 0) recomp_stop(1, "animation:ambiguous-draw-binding");
        found = binding;
    }
    if (found) *output = found->replay;
    else {
        unsigned actor = experiment_actor();
        uint32_t record = seam_record(object, actor);
        if (count != 1) return false;
        unsigned joint = record ? *(const uint8_t *)recomp_memory_i8(record+12u) :
            attachment_bone(object, actor);
        if (joint >= 32 || memcmp(worlds[0],
            recomp_memory(0x004d3650u+actor*0x800u+joint*64u, 64), 64) != 0) return false;
        memset(output, 0, sizeof *output);
        output->frame = experiment_frame(); output->actor = actor;
        output->recipe = UINT32_MAX; output->count = 1;
        for (unsigned phase = 0; phase < RECOMP_POSE_REPLAY_SAMPLES; ++phase)
            memcpy(output->palettes[phase][0], experiment_bones[phase][joint].m, 64);
    }
    return true;
#else
    (void)worlds; (void)count; (void)output;
    return false;
#endif
}

/* The stream contains guest destination addresses, but replay writes only its
   owned draw buffer. Rig controls and point/normal pairs remain immutable. */
void *recomp_animation_probe_pose_vertices(const struct RecompD3dPresenterDrawCommand *draw)
{
#ifdef RECOMP_FULL_PROGRAM
    if (!draw || !draw->pose_replay || !experiment_ready) return NULL;
    unsigned actor = experiment_actor();
    uint32_t record = seam_record(*recomp_memory_u32(0x00a2479cu), actor);
    if (!record) return NULL;
    if (*recomp_memory_u32(record+24) || *recomp_memory_u32(record+28))
        recomp_stop(1, "animation:seam-link-stream-unimplemented");
    unsigned destination = *(const uint8_t *)recomp_memory_i8(record+12);
    unsigned source = *(const uint8_t *)recomp_memory_i8(record+13);
    if (destination >= 32 || source >= 32) recomp_stop(1, "animation:seam-bone-index");
    uintptr_t base = (uintptr_t)recomp_memory(0, RECOMP_XBOX_RAM_SIZE);
    uintptr_t first = (uintptr_t)draw->vertex_bytes-base;
    size_t size = (size_t)draw->vertex_count*draw->vertex_stride;
    if (first >= RECOMP_XBOX_RAM_SIZE || size > RECOMP_XBOX_RAM_SIZE-first)
        recomp_stop(1, "animation:seam-vertex-span");
    uint8_t *vertices = malloc(size*RECOMP_POSE_REPLAY_SAMPLES);
    if (!vertices) recomp_stop(1, "animation:seam-allocation");
    float reference[10][4];
    uint32_t reference_address = *recomp_memory_u32(record+8);
    recomp_guest_load(reference, reference_address, sizeof reference);
    for (unsigned phase = 0; phase < RECOMP_POSE_REPLAY_SAMPLES; ++phase) {
        uint8_t *output = vertices+size*phase;
        memcpy(output, draw->vertex_bytes, size);
        RecompBoneMatrix relative, matrix;
        RecompSeamBasis basis;
        if (!recomp_animation_seam_relative(&experiment_bones[phase][source],
                &experiment_bones[phase][destination], &relative) ||
            !recomp_animation_seam_basis(reference, &relative, &basis))
            recomp_stop(1, "animation:seam-basis");
        matrix = relative;
        uint32_t cursor = *recomp_memory_u32(record+4), point = reference_address+sizeof reference;
        unsigned words = 0;
        while (*recomp_memory_u32(cursor)) {
            uint32_t command = *recomp_memory_u32(cursor); cursor += 4;
            if (command < 16) {
                float coefficients[4];
                recomp_guest_load(coefficients, 0x00333770u+command*16, sizeof coefficients);
                if (!recomp_animation_seam_matrix(&basis, coefficients, &matrix))
                    recomp_stop(1, "animation:seam-matrix");
            }
            do {
                float input[8], vertex[6];
                recomp_guest_load(input, point, sizeof input); point += sizeof input;
                recomp_animation_seam_vertex(&matrix, input, input+4, vertex);
                uint32_t address;
                while ((address = *recomp_memory_u32(cursor)) != UINT32_MAX) {
                    address &= RECOMP_XBOX_RAM_SIZE-1;
                    if (address >= first && address-first <= size && size-(address-first) >= sizeof vertex)
                        memcpy(output+address-first, vertex, sizeof vertex);
                    cursor += 4;
                    if (++words > 100000) recomp_stop(1, "animation:seam-stream-limit");
                }
                cursor += 4;
                if (++words > 100000) recomp_stop(1, "animation:seam-stream-limit");
            } while (*recomp_memory_u32(cursor) != UINT32_MAX);
            cursor += 4;
        }
    }
    return vertices;
#else
    (void)draw;
    return NULL;
#endif
}

unsigned recomp_animation_probe_object_bone(uint32_t object, unsigned actor, uint32_t *record)
{
#ifdef RECOMP_FULL_PROGRAM
    *record = seam_record(object, actor);
    return *record ? *(const uint8_t *)recomp_memory_i8(*record+12) : attachment_bone(object, actor);
#else
    (void)object; (void)actor; *record = 0; return UINT32_MAX;
#endif
}

RecompFunction recomp_animation_probe_lookup_manual(uint32_t address)
{
#ifdef RECOMP_FULL_PROGRAM
    switch (address) {
    /* Temporary SDK draw-library seam: observe the game's indirect callback
       provenance, then execute the original once. Library replacement remains
       open; the plain pose/palette models do not depend on these callbacks. */
    case 0x0017d520u: return (experiment_frame() || recomp_animation_split_enabled()) ? draw_rigid : NULL;
    case 0x0017d670u: return (experiment_frame() || recomp_animation_split_enabled()) ? draw_object : NULL;
    case 0x000af050u: case 0x000aeef0u: case 0x000af5c0u:
    case 0x000636d0u: case 0x000b01e0u: case 0x00023b10u:
        if (getenv("RECOMP_ANIMATION_DISPATCH_TRACE") == NULL &&
            getenv("RECOMP_SKELETON_VERIFY") == NULL && experiment_frame() == 0 && state_capture_frame() == 0 && !recomp_animation_split_enabled()) return NULL;
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
