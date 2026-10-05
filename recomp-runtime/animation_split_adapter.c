#include "animation_split_adapter.h"
#include "animation_probe.h"
#include "d3d_frame_adapter.h"
#include "runtime.h"
#include "stop_report.h"

#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

bool recomp_animation_split_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = recomp_split_rate(getenv("RECOMP_SPLIT_RATE")) != 0;
    return enabled != 0;
}

#ifdef RECOMP_FULL_PROGRAM
static RecompVisualPose pairs[4];
static uint32_t frames[4];
static bool ready[4];
typedef struct SplitBinding {
    uint32_t object, actor, joint, count;
    uint8_t recipe[16];
    float offsets[24][4];
    RecompBoneMatrix initial, original[4];
} SplitBinding;
static SplitBinding bindings[512];
static unsigned binding_count;
static uint32_t binding_frame;
typedef struct CameraPair {
    RecompVisualCamera inputs[2];
    float view[16], projection[16];
    uint8_t flags[16];
    uint32_t frame;
    bool ready;
} CameraPair;
static CameraPair cameras[5];

static SplitBinding *add_binding(uint32_t frame)
{
    if (frame != binding_frame) { binding_count = 0; binding_frame = frame; }
    if (binding_count == 512) recomp_stop(1, "split:binding-capacity");
    SplitBinding *binding = bindings+binding_count++;
    memset(binding, 0, sizeof *binding);
    return binding;
}
#endif

void recomp_animation_split_capture(unsigned actor, uint32_t frame,
    const RecompVisualPose *snapshot)
{
#ifdef RECOMP_FULL_PROGRAM
    if (actor >= 4 || !recomp_animation_split_enabled()) return;
    RecompVisualPose *pair = pairs+actor;
    ready[actor] = frames[actor]+1 == frame &&
        memcmp(&pair->tables, &snapshot->tables, sizeof pair->tables) == 0;
    pair->channels[0] = pair->channels[1];
    pair->targets[0] = pair->targets[1];
    memcpy(pair->endpoints[0], pair->endpoints[1], sizeof pair->endpoints[0]);
    pair->tables = snapshot->tables;
    memcpy(pair->groups, snapshot->groups, sizeof pair->groups);
    pair->channels[1] = snapshot->channels[0];
    pair->targets[1] = snapshot->targets[0];
    memcpy(pair->endpoints[1], snapshot->endpoints[0], sizeof pair->endpoints[1]);
    frames[actor] = frame;
#else
    (void)actor; (void)frame; (void)snapshot;
#endif
}

void recomp_animation_split_palette(unsigned actor, uint32_t frame, uint32_t object,
    unsigned count, const uint8_t recipe[16], const float offsets[24][4],
    const RecompBoneMatrix *initial, const RecompBoneMatrix original[4])
{
#ifdef RECOMP_FULL_PROGRAM
    if (actor >= 4 || !ready[actor] || frames[actor] != frame) return;
    SplitBinding *binding = add_binding(frame);
    binding->object = object; binding->actor = actor;
    binding->joint = UINT32_MAX; binding->count = count;
    memcpy(binding->recipe, recipe, sizeof binding->recipe);
    memcpy(binding->offsets, offsets, sizeof binding->offsets);
    binding->initial = *initial;
    memcpy(binding->original, original, count*sizeof *original);
#else
    (void)actor; (void)frame; (void)object; (void)count; (void)recipe;
    (void)offsets; (void)initial; (void)original;
#endif
}

void recomp_animation_split_rigid(void)
{
#ifdef RECOMP_FULL_PROGRAM
    if (!recomp_animation_split_enabled()) return;
    uint32_t context = recomp_runtime.registers.ebx, caller = *recomp_ebp_register();
    if (context > RECOMP_XBOX_RAM_SIZE-0x2cu || caller < 8 || caller >= RECOMP_XBOX_RAM_SIZE) return;
    uint32_t bank = *recomp_memory_u32(context+0x1c), bone = *recomp_memory_u32(caller-8);
    unsigned actor = (bank-0x004d3650u)/0x800u;
    uint32_t frame = recomp_d3d_frame_adapter_swap_counter();
    if (actor >= 4 || bank != 0x004d3650u+actor*0x800u || !ready[actor] ||
        frames[actor] != frame || bone < bank || bone >= bank+0x800u || (bone-bank)%64) return;
    if (memcmp(recomp_memory(bone, 64), recomp_memory(0x00a24190u, 64), 64)) return;
    SplitBinding *binding = add_binding(frame);
    binding->object = recomp_runtime.registers.ecx; binding->actor = actor;
    binding->joint = (bone-bank)/64; binding->count = 1;
    recomp_guest_load(binding->original, bone, 64);
#endif
}

void recomp_animation_split_camera(uint32_t address)
{
#ifdef RECOMP_FULL_PROGRAM
    if (!recomp_animation_split_enabled() || address < 0x0041a800u) return;
    unsigned slot = (address-0x0041a800u)/0xae0u;
    if (slot >= 5 || address != 0x0041a800u+slot*0xae0u) return;
    uint32_t frame = recomp_d3d_frame_adapter_swap_counter();
    CameraPair *camera = cameras+slot;
    uint8_t flags[16];
    recomp_guest_load(flags, address+0xad0, sizeof flags);
    if (camera->frame != frame) {
        camera->ready = camera->frame+1 == frame && memcmp(flags, camera->flags, sizeof flags) == 0;
        camera->inputs[0] = camera->inputs[1];
        camera->frame = frame;
    }
    memcpy(camera->flags, flags, sizeof flags);
    RecompVisualCamera *input = camera->inputs+1;
    recomp_guest_load(input->eye, address+0x180, 12);
    recomp_guest_load(input->target, address+0x190, 12);
    recomp_guest_load(&input->fov, address+0x1b0, 4);
    recomp_guest_load(&input->roll, address+0x1b4, 4);
    recomp_guest_load(&input->near_z, address+0x2ac, 7*4);
    recomp_guest_load(camera->view, address+0x50, 64);
    recomp_guest_load(camera->projection, address+0x90, 64);
    input->aspect = camera->projection[5]*input->scale_x/(camera->projection[0]*input->scale_y);
    float view[16], projection[16];
    if (!recomp_animation_camera(input, view, projection)) { camera->ready = false; return; }
    float error = 0;
    for (unsigned i = 0; i < 16; ++i) {
        error = fmaxf(error, fabsf(view[i]-camera->view[i]));
        error = fmaxf(error, fabsf(projection[i]-camera->projection[i]));
    }
    if (error > 0.0001f) {
        fprintf(stderr, "recomp split camera mismatch: frame=%u slot=%u error=%.9g roll=%.9g eye=%.9g,%.9g,%.9g target=%.9g,%.9g,%.9g\n", frame, slot, error, input->roll, input->eye[0], input->eye[1], input->eye[2], input->target[0], input->target[1], input->target[2]);
        recomp_stop(1, "split:camera-mismatch");
    }
#else
    (void)address;
#endif
}

void *recomp_animation_split_draw(const RecompD3dPresenterDrawCommand *draw,
    const float worlds[4][16], unsigned count, const float view[16],
    const float projection[16], uint32_t *size)
{
#ifdef RECOMP_FULL_PROGRAM
    if (!recomp_animation_split_enabled() || !draw || !count || count > 4 || !size) return NULL;
    uint32_t frame = recomp_d3d_frame_adapter_swap_counter();
    static uint32_t report_frame;
    static unsigned actor_draws[4], camera_draws, ball_draws, seam_draws;
    if (report_frame != frame) {
        if (report_frame && report_frame%300 == 0)
            fprintf(stderr, "recomp split bindings: frame=%u actors=%u,%u,%u,%u cameras=%u balls=%u seams=%u\n",
                report_frame, actor_draws[0], actor_draws[1], actor_draws[2], actor_draws[3],
                camera_draws, ball_draws, seam_draws);
        memset(actor_draws, 0, sizeof actor_draws);
        camera_draws = ball_draws = seam_draws = 0; report_frame = frame;
    }
    uint32_t object = *recomp_memory_u32(0x00a2479cu);
    const SplitBinding *found = NULL;
    for (unsigned i = 0; binding_frame == frame && i < binding_count; ++i) {
        const SplitBinding *b = bindings+i;
        if (b->object == object && b->count == count &&
            memcmp(b->original, worlds, count*64) == 0) {
            if (found && memcmp(found, b, sizeof *b)) recomp_stop(1, "split:ambiguous-binding");
            found = b;
        }
    }
    unsigned actor = found ? found->actor : UINT32_MAX;
    unsigned joint = found ? found->joint : UINT32_MAX;
    uint32_t seam = 0;
    if (count == 1) for (unsigned i = 0; i < 4; ++i) {
        if (!ready[i] || frames[i] != frame) continue;
        uint32_t record;
        unsigned bone = recomp_animation_probe_object_bone(object, i, &record);
        if (bone >= 32 || memcmp(worlds[0], recomp_memory(0x004d3650u+i*0x800u+bone*64u, 64), 64)) continue;
        actor = i; joint = bone; seam = record; break;
    }
    const CameraPair *camera = NULL;
    for (unsigned i = 0; i < 5; ++i) {
        if (cameras[i].ready && cameras[i].frame == frame &&
            memcmp(view, cameras[i].view, 64) == 0 && memcmp(projection, cameras[i].projection, 64) == 0) {
            camera = cameras+i; break;
        }
    }
    unsigned ball = 0;
    if (count == 1 && *(const uint8_t *)recomp_memory_i8(0x0041831bu)) {
        if (object == *recomp_memory_u32(0x00b2d658u)) ball = 1;
        else if (object == *recomp_memory_u32(0x004ca4b0u)) ball = 2;
    }
    if (actor >= 4 && !camera && !ball) return NULL;
    size_t bytes = sizeof(RecompSplitDraw)+(seam ? (size_t)draw->vertex_count*sizeof(RecompSplitVertex) : 0);
    if (bytes > RECOMP_XBOX_RAM_SIZE) recomp_stop(1, "split:payload-size");
    RecompSplitDraw *split = calloc(1, bytes);
    if (!split) recomp_stop(1, "split:allocation");
    split->frame = frame; split->actor = actor; split->joint = joint; split->count = count;
    split->pose = actor < 4;
    if (split->pose) split->visual = pairs[actor];
    if (camera) {
        split->camera = true;
        memcpy(split->cameras, camera->inputs, sizeof split->cameras);
    }
    memcpy(split->worlds, worlds, count*64);
    memcpy(split->view, view, sizeof split->view);
    memcpy(split->projection, projection, sizeof split->projection);
    if (ball) {
        unsigned latest = *(const uint8_t *)recomp_memory_i8(0x00a1783cu)&7u;
        split->ball = ball;
        recomp_guest_load(&split->ball_base, 0x004d5680u, 64);
        for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
            unsigned index = (latest+endpoint+7)&7;
            uint32_t record = 0x00a17718u+index*0x24u;
            recomp_guest_load(split->ball_position[endpoint], record, 12);
            for (unsigned axis = 0; axis < 3; ++axis) {
                int32_t angle = (int32_t)*recomp_memory_u32(record+16+axis*4);
                split->ball_angles[endpoint][axis] = (float)angle*(3.14159265358979323846f/32768.0f);
            }
        }
        float expected[16];
        recomp_split_ball_matrix(split, 1, expected);
        float error = 0;
        for (unsigned i = 0; i < 16; ++i) error = fmaxf(error, fabsf(expected[i]-worlds[0][i]));
        // Historical trail draws share the mesh. Only the current ball matches.
        if (error > 0.0001f) split->ball = 0;
    }
    if (found) {
        memcpy(split->recipe, found->recipe, sizeof split->recipe);
        memcpy(split->offsets, found->offsets, sizeof split->offsets);
        split->initial = found->initial;
    }
    if (seam) {
        if (*recomp_memory_u32(seam+24) || *recomp_memory_u32(seam+28))
            recomp_stop(1, "split:seam-links-unimplemented");
        split->seam = true;
        split->seam_destination = *(const uint8_t *)recomp_memory_i8(seam+12);
        split->seam_source = *(const uint8_t *)recomp_memory_i8(seam+13);
        uint32_t point = *recomp_memory_u32(seam+8);
        recomp_guest_load(split->reference, point, sizeof split->reference);
        point += sizeof split->reference;
        uintptr_t first = (uintptr_t)draw->vertex_bytes-(uintptr_t)recomp_memory(0, RECOMP_XBOX_RAM_SIZE);
        size_t span = (size_t)draw->vertex_count*draw->vertex_stride;
        RecompSplitVertex *vertices = (RecompSplitVertex *)(split+1), vertex = {0};
        vertex.mode = 2;
        uint32_t cursor = *recomp_memory_u32(seam+4);
        unsigned words = 0;
        while (*recomp_memory_u32(cursor)) {
            unsigned command = *recomp_memory_u32(cursor); cursor += 4;
            if (command < 16) {
                vertex.mode = 1;
                recomp_guest_load(vertex.coefficients, 0x00333770u+command*16, 16);
            }
            do {
                recomp_guest_load(vertex.position, point, 12);
                recomp_guest_load(vertex.normal, point+16, 12); point += 32;
                uint32_t address;
                while ((address = *recomp_memory_u32(cursor)) != UINT32_MAX) {
                    address &= RECOMP_XBOX_RAM_SIZE-1;
                    if (address >= first && address-first < span) {
                        if ((address-first)%draw->vertex_stride) recomp_stop(1, "split:seam-vertex-alignment");
                        vertices[(address-first)/draw->vertex_stride] = vertex;
                    }
                    cursor += 4;
                    if (++words > 100000) recomp_stop(1, "split:seam-stream-limit");
                }
                cursor += 4;
                if (++words > 100000) recomp_stop(1, "split:seam-stream-limit");
            } while (*recomp_memory_u32(cursor) != UINT32_MAX);
            cursor += 4;
        }
    }
    if (split->pose) ++actor_draws[actor];
    camera_draws += split->camera; ball_draws += split->ball != 0; seam_draws += split->seam;
    *size = (uint32_t)bytes;
    return split;
#else
    (void)draw; (void)worlds; (void)count; (void)view; (void)projection; (void)size;
    return NULL;
#endif
}
