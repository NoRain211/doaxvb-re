#include "body_joints_adapter.h"
#include "body_joints.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The girls' joint pieces, frame by frame (docs/body-joints.md).

   Each girl n has up to 32 joint records at 0x004CA640 + n * 0x400 (0x20
   bytes each), counted by the byte at 0x004CB644 + n, built from her body
   model when she is loaded (0x000672B0..0x0006749A):
     +0x00 the piece's vertex buffer       +0x04 its interior list
     +0x08 its frame data                  +0x0C bone a   +0x0D bone b
     +0x0E skip flags (1 or 2: joints on a foot, bone 3 or 9, 2 when the
           model marks it; 4: joints on the toes, bone 18 or 22)                        +0x10, +0x14 the buffers of the
     +0x18 copy list and +0x1C transform list (offsets made pointers at load)

   0x00067540 (the girl in EAX; two stack arguments, the bone matrix tables
   for bone b and bone a, 64 bytes a bone) updates every record of hers that
   the girl's mode does not skip (mode byte 0x004C9744 + n: 1 skips flags 6,
   2 skips 5, otherwise 2; the mask is kept at 0x004CB640 + n). For each:
   the current matrix (0x00A24190) becomes bone b's matrix times bone a's,
   then 0x00067630 bends the piece:

   1. copy list: groups of a source vertex and the vertices that take it,
      each group ended by -1, the list by a -1 where a source would be; a
      source with its low bit set gives its position only, else position
      and normal (at +0x0C).
   2. transform list: the same groups; the source's position and normal
      (at +0x18) go through the current matrix (scratch 0x004CA4E0,
      0x004CA4F0) into every vertex of the group.
   3. frames (scratch 0x004CA500): recomp_joint_frames from the 0xA0 bytes
      at +0x08.
   4. interior list: a step k (0 ends the list); for k < 16 the frame at
      that step (recomp_joint_blend_frame, scratch 0x004CA600) becomes the
      current matrix, for larger k the last one stays; then groups as
      above, each placing the next 32-byte vertex after the frame data
      (position, normal) through it into every vertex of the group; the
      groups end with an extra -1.

   Registered for 0x00067540, which the recipe routes through the manual
   lookup; it calls no game code. */
enum {
    GIRLS = 4u,
    RECORDS = 0x004CA640u,
    RECORD_STRIDE = 0x400u,
    RECORD_SIZE = 0x20u,
    RECORD_COUNT = 0x004CB644u,
    SKIP_MASK = 0x004CB640u,
    GIRL_MODE = 0x004C9744u,
    MATRIX = 0x00A24190u,
    SCRATCH_POSITION = 0x004CA4E0u,
    SCRATCH_NORMAL = 0x004CA4F0u,
    SCRATCH_FRAMES = 0x004CA500u,
    SCRATCH_ROWS = 0x004CA600u,
    FRAME_DATA_SIZE = 0xA0u,
    END = 0xFFFFFFFFu,
    UPDATE_JOINTS = 0x00067540u,
};

#ifdef RECOMP_FULL_PROGRAM

static uint32_t load_u(uint32_t address) { return *recomp_memory_u32(address); }
static uint8_t load_b(uint32_t address) { return *recomp_memory(address, 1u); }
static void store_b(uint32_t address, uint8_t value) { *recomp_memory(address, 1u) = value; }

static void load_floats(uint32_t address, float *out, unsigned count)
{
    memcpy(out, recomp_memory(address, count * 4u), count * 4u);
}

static void store_floats(uint32_t address, const float *in, unsigned count)
{
    memcpy(recomp_memory(address, count * 4u), in, count * 4u);
}

static void copy_words(uint32_t to, uint32_t from, unsigned count)
{
    memmove(recomp_memory(to, count * 4u), recomp_memory(from, count * 4u), count * 4u);
}

/* Step 1: vertices that take another's position (and normal) as it is. */
static void copy_list(uint32_t list)
{
    if (list == 0u) return;
    uint32_t source = load_u(list);
    list += 4u;
    while (source != END) {
        const bool position_only = (source & 1u) != 0u;
        source &= ~1u;
        uint32_t target = load_u(list);
        list += 4u;
        do {
            copy_words(target, source, 3u);
            if (!position_only) copy_words(target + 12u, source + 12u, 3u);
            target = load_u(list);
            list += 4u;
        } while (target != END);
        source = load_u(list);
        list += 4u;
    }
}

/* Step 2: vertices that take another's position and normal through the
   current matrix. */
static void transform_list(uint32_t list)
{
    if (list == 0u) return;
    uint32_t source = load_u(list);
    list += 4u;
    while (source != END) {
        const bool position_only = (source & 1u) != 0u;
        source &= ~1u;
        float m[16], point[3], direction[3], position[4], normal[4];
        load_floats(MATRIX, m, 16u);
        load_floats(source, point, 3u);
        load_floats(source + 0x18u, direction, 3u);
        recomp_joint_transform(m, point, position);
        recomp_joint_rotate(m, direction, normal);
        store_floats(SCRATCH_POSITION, position, 4u);
        store_floats(SCRATCH_NORMAL, normal, 4u);
        uint32_t target = load_u(list);
        list += 4u;
        while (target != END) {
            copy_words(target, SCRATCH_POSITION, 3u);
            if (!position_only) copy_words(target + 12u, SCRATCH_NORMAL, 3u);
            target = load_u(list);
            list += 4u;
        }
        source = load_u(list);
        list += 4u;
    }
}

/* Steps 3 and 4. */
static void interior(uint32_t data, uint32_t list)
{
    float m[16], frames[64], points[40];
    load_floats(MATRIX, m, 16u);
    load_floats(SCRATCH_FRAMES, frames, 64u);
    load_floats(data, points, 40u);
    recomp_joint_frames(m, points, frames);
    store_floats(SCRATCH_FRAMES, frames, 64u);

    uint32_t vertex = data + FRAME_DATA_SIZE;
    uint32_t step = load_u(list);
    list += 4u;
    while (step != 0u) {
        if (step < 16u) {
            float weights[4], rows[16];
            recomp_joint_weights(step, weights);
            recomp_joint_blend_frame(frames, weights, rows);
            store_floats(SCRATCH_ROWS, rows, 16u);
            store_floats(MATRIX, rows, 16u);
        }
        uint32_t target = load_u(list);
        list += 4u;
        do {
            float source[8], position[4], normal[4];
            load_floats(MATRIX, m, 16u);
            load_floats(vertex, source, 8u);
            recomp_joint_place(m, source, position, normal);
            store_floats(SCRATCH_POSITION, position, 4u);
            store_floats(SCRATCH_NORMAL, normal, 4u);
            vertex += 0x20u;
            do {
                copy_words(target, SCRATCH_POSITION, 3u);
                copy_words(target + 12u, SCRATCH_NORMAL, 3u);
                target = load_u(list);
                list += 4u;
            } while (target != END);
            target = load_u(list);
            list += 4u;
        } while (target != END);
        step = load_u(list);
        list += 4u;
    }
}

/* 0x00067630: one joint piece. */
static void bend(uint32_t record)
{
    copy_list(load_u(record + 0x18u));
    transform_list(load_u(record + 0x1Cu));
    interior(load_u(record + 0x08u), load_u(record + 0x04u));
}

/* 0x00067540: every joint piece of the girl in EAX. */
static void update_joints(void)
{
    const uint32_t girl = recomp_runtime.registers.eax;
    const uint32_t esp = recomp_runtime.registers.esp;
    const uint32_t bone_b_table = load_u(esp + 4u), bone_a_table = load_u(esp + 8u);
    const uint8_t mode = load_b(GIRL_MODE + girl);
    const uint8_t skip = mode == 1u ? 6u : mode == 2u ? 5u : 2u;
    store_b(SKIP_MASK + girl, skip);
    const unsigned count = load_b(RECORD_COUNT + girl);
    uint32_t record = RECORDS + girl * RECORD_STRIDE;
    for (unsigned i = 0; i < count; ++i, record += RECORD_SIZE) {
        if (load_b(record + 0x0Eu) & skip) continue;
        float a[16], b[16], m[16];
        load_floats(bone_a_table + (uint32_t)load_b(record + 0x0Cu) * 64u, a, 16u);
        load_floats(bone_b_table + (uint32_t)load_b(record + 0x0Du) * 64u, b, 16u);
        recomp_joint_multiply(m, b, a);
        store_floats(MATRIX, m, 16u);
        bend(record);
    }
    recomp_runtime.registers.esp = esp + 4u;
}

/* RECOMP_BODY_JOINTS_VERIFY=1 (a check, not for play): each call runs the
   generated code, then from the same state this file's, and compares
   everything either writes. Not compared: the w lanes of the rows rebuilt
   against others (0x004CA60C, 0x004CA61C), of the current matrix's copies
   of them (0x00A2419C, 0x00A241AC) and of the placed vertex (0x004CA4EC,
   0x004CA4FC). The game subtracts a stack word it never wrote there
   (0x0017B080), and no vertex reads them. */
void sub_00067540(void);

typedef struct Region { uint32_t address, size; } Region;

static bool in_ram(uint32_t address, uint32_t size)
{
    const uint32_t top = address >> 28;
    return (top == 0u || top == 8u) && (address & 0x0FFFFFFFu) + size <= 0x04000000u;
}

/* A reader over a guest list of words. */
typedef struct Cursor { uint32_t at; } Cursor;
static uint32_t next(Cursor *c) { const uint32_t w = load_u(c->at); c->at += 4u; return w; }

static void add(Region *out, size_t *n, size_t limit, uint32_t target)
{
    if (*n < limit && in_ram(target, 24u)) out[(*n)++] = (Region){target, 24u};
}

/* The vertices a copy or transform list writes (its groups' targets). */
static void group_targets(uint32_t list, Region *out, size_t *n, size_t limit)
{
    if (list == 0u || !in_ram(list, 4u)) return;
    Cursor c = {list};
    for (uint32_t source = next(&c); source != END && *n < limit; source = next(&c))
        for (uint32_t target = next(&c); target != END; target = next(&c)) add(out, n, limit, target);
}

/* The vertices an interior list writes. */
static void interior_targets(uint32_t list, Region *out, size_t *n, size_t limit)
{
    if (list == 0u || !in_ram(list, 4u)) return;
    Cursor c = {list};
    for (uint32_t step = next(&c); step != 0u && *n < limit; step = next(&c)) {
        uint32_t target = next(&c);
        do {
            do { add(out, n, limit, target); target = next(&c); } while (target != END);
            target = next(&c);
        } while (target != END);
    }
}

static size_t regions(unsigned girl, Region *out, size_t limit)
{
    size_t n = 0;
    out[n++] = (Region){MATRIX, 64u};
    out[n++] = (Region){SCRATCH_POSITION, 0x160u};
    out[n++] = (Region){SKIP_MASK + girl, 1u};
    const unsigned count = load_b(RECORD_COUNT + girl);
    for (unsigned i = 0; i < count && i < 32u; ++i) {
        const uint32_t record = RECORDS + girl * RECORD_STRIDE + i * RECORD_SIZE;
        group_targets(load_u(record + 0x18u), out, &n, limit);
        group_targets(load_u(record + 0x1Cu), out, &n, limit);
        interior_targets(load_u(record + 0x04u), out, &n, limit);
    }
    return n;
}

static bool ignored(uint32_t address)
{
    return address == 0x004CA60Cu || address == 0x004CA61Cu || address == 0x00A2419Cu ||
        address == 0x00A241ACu || address == 0x004CA4ECu || address == 0x004CA4FCu;
}

static void verified(void)
{
    static uint64_t calls, differing;
    enum { LIMIT = 8192 };
    static Region list[LIMIT];
    const unsigned girl = recomp_runtime.registers.eax & 3u;
    const size_t count = regions(girl, list, LIMIT);
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) total += list[i].size;
    uint8_t *before = malloc(total), *theirs = malloc(total), *mine = malloc(total);
    if (before == NULL || theirs == NULL || mine == NULL) {
        free(before); free(theirs); free(mine);
        update_joints();
        return;
    }
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) { memcpy(before + at, recomp_memory(list[i].address, list[i].size), list[i].size); at += list[i].size; }
    const RecompRegisters entry = recomp_runtime.registers;
    RecompFpuContext fpu;
    recomp_fpu_context_save(&fpu);

    sub_00067540();
    const RecompRegisters after_theirs = recomp_runtime.registers;
    at = 0;
    for (size_t i = 0; i < count; ++i) { memcpy(theirs + at, recomp_memory(list[i].address, list[i].size), list[i].size); at += list[i].size; }

    at = 0;
    for (size_t i = 0; i < count; ++i) { memcpy(recomp_memory(list[i].address, list[i].size), before + at, list[i].size); at += list[i].size; }
    recomp_runtime.registers = entry;
    recomp_fpu_context_restore(&fpu);

    update_joints();
    at = 0;
    for (size_t i = 0; i < count; ++i) { memcpy(mine + at, recomp_memory(list[i].address, list[i].size), list[i].size); at += list[i].size; }

    ++calls;
    bool same = after_theirs.esp == recomp_runtime.registers.esp;
    at = 0;
    for (size_t i = 0; i < count; ++i) {
        for (uint32_t k = 0; k + 4u <= list[i].size; k += 4u) {
            if (memcmp(theirs + at + k, mine + at + k, 4u) == 0 || ignored(list[i].address + k)) continue;
            same = false;
            if (differing < 40u) {
                uint32_t a = 0, b = 0;
                memcpy(&a, theirs + at + k, 4u);
                memcpy(&b, mine + at + k, 4u);
                fprintf(stderr, "body joints verify: call %llu girl %u differs at 0x%08x: game %08x mine %08x\n",
                    (unsigned long long)calls, girl, list[i].address + k, a, b);
            }
        }
        if (list[i].size % 4u && memcmp(theirs + at, mine + at, list[i].size) != 0) same = false;
        at += list[i].size;
    }
    if (!same) ++differing;
    if (calls % 600u == 0u)
        fprintf(stderr, "body joints verify: %llu calls, %llu differing (%zu regions last call)\n",
            (unsigned long long)calls, (unsigned long long)differing, count);
    free(before); free(theirs); free(mine);
}

static void update_joints_entry(void)
{
    static int verify = -1;
    if (verify < 0) verify = getenv("RECOMP_BODY_JOINTS_VERIFY") != NULL;
    if (verify) verified();
    else update_joints();
}

RecompFunction recomp_body_joints_lookup_manual(uint32_t guest_address)
{
    /* Seam: the game's per-frame bend of every joint piece of a girl
       (manual call target in the recipe); docs/body-joints.md. */
    return guest_address == UPDATE_JOINTS ? update_joints_entry : NULL;
}
#else
RecompFunction recomp_body_joints_lookup_manual(uint32_t guest_address)
{
    (void)guest_address;
    return NULL;
}
#endif
