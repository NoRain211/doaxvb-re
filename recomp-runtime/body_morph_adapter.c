#include "body_morph_adapter.h"
#include "body_morph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The girls' morphing parts, one record at a time (docs/body-morph.md).

   A blend record (0x24 bytes; the face's, the hands', the chest's and the
   swimsuit's records of each girl, which 0x00064300 visits in turn):
     +0x00 morph set      +0x04 model        +0x08 shape A    +0x0C shape B
     +0x10 shape C        +0x14 weight A     +0x18 weight B   +0x1C weight C
     +0x20 the model's vertex buffer number  +0x21 kind (1: mirrored in x)
     +0x22 dirty
   A morph set: +0x08 positions to write, +0x0C normals to write, +0x10 its
   shapes (8 bytes a shape: positions, normals), +0x14 the positions' byte
   offsets in the vertex buffer, +0x18 the normals'.

   0x00064FD0 (the record in ESI) does nothing unless the record is dirty;
   it clears the flag, and with a model and a set locks the model's vertex
   buffer (0x001E4D00, storing the buffer at 0x004C9C38), picks a blend from
   the table at 0x0033F0D0 (one, two or three shapes; two entries a blend,
   the second mirrored) and writes the positions, then the normals.

   Registered for 0x00064FD0, which the recipe routes through the manual
   lookup. Seam: the D3D vertex buffer lock (0x001E4D00) is library code
   and is called as it is. A table entry other than the six blends known
   here (the table is constant data) runs the generated function instead. */
enum {
    BLEND_RECORD = 0x00064FD0u,
    BLEND_TABLE = 0x0033F0D0u,
    LOCKED_BUFFER = 0x004C9C38u,
    VERTEX_BUFFER_LOCK = 0x001E4D00u,
    ADDRESS_MASK = 0x0FFFFFFFu,
};

#ifdef RECOMP_FULL_PROGRAM

void sub_00064FD0(void);

static uint32_t load_u(uint32_t address) { return *recomp_memory_u32(address); }
static void store_u(uint32_t address, uint32_t value) { *recomp_memory_u32(address) = value; }
static uint8_t load_b(uint32_t address) { return *recomp_memory(address, 1u); }
static void store_b(uint32_t address, uint8_t value) { *recomp_memory(address, 1u) = value; }

static float load_f(uint32_t address)
{
    float value;
    memcpy(&value, recomp_memory(address, 4u), 4u);
    return value;
}

/* The blends in the table, by kind, then mirrored. */
static const uint32_t known_blends[3][2] = {
    {0x00065890u, 0x000658D0u},
    {0x00065910u, 0x00065980u},
    {0x000659F0u, 0x00065A80u},
};

/* The last lock (for the check). */
static uint32_t last_locked_resource, last_locked_buffer;

/* 0x001E4D00(resource, 0): the buffer's vertices, as the game locks them. */
static uint32_t lock_vertex_buffer(uint32_t resource)
{
    const RecompRegisters saved = recomp_runtime.registers;
    RecompFpuContext fpu;
    recomp_fpu_context_save(&fpu);
    recomp_runtime.registers.esp -= 12u;
    store_u(recomp_runtime.registers.esp, 0u);
    store_u(recomp_runtime.registers.esp + 4u, resource);
    store_u(recomp_runtime.registers.esp + 8u, 0u);
    recomp_dispatch(VERTEX_BUFFER_LOCK);
    const uint32_t buffer = recomp_runtime.registers.eax;
    recomp_runtime.registers = saved;
    recomp_fpu_context_restore(&fpu);
    last_locked_resource = resource;
    last_locked_buffer = buffer;
    return buffer;
}

/* 0x00064FD0: one record. */
static void blend_record(void)
{
    const uint32_t record = recomp_runtime.registers.esi;
    if (load_b(record + 0x22u) != 0u) {
        const uint32_t a = load_u(record + 0x08u), b = load_u(record + 0x0Cu), c = load_u(record + 0x10u);
        const float wa = load_f(record + 0x14u), wb = load_f(record + 0x18u), wc = load_f(record + 0x1Cu);
        const RecompMorphKind kind = recomp_morph_kind(a, b, wc);
        const uint8_t variant = load_b(record + 0x21u);
        const uint32_t blend = load_u(BLEND_TABLE + (2u * (uint32_t)kind + variant) * 4u);
        if (variant > 1u || blend != known_blends[kind][variant]) {
            sub_00064FD0();                                 /* a blend this file does not know */
            return;
        }
        const uint32_t model = load_u(record + 0x04u);
        store_b(record + 0x22u, 0u);
        const uint32_t set = load_u(record);
        if (model != 0u && set != 0u) {
            const uint32_t resource = load_u(model + 0x24u + (uint32_t)load_b(record + 0x20u) * 16u);
            store_u(LOCKED_BUFFER, resource);
            const uint32_t buffer = lock_vertex_buffer(resource) & ADDRESS_MASK;
            for (unsigned normals = 0; normals < 2u; ++normals) {
                const uint32_t count = load_u(set + 0x08u + 4u * normals);
                if ((int32_t)count <= 0) continue;
                const uint32_t *offsets = (const uint32_t *)(const void *)recomp_memory(load_u(set + 0x14u + 4u * normals), (size_t)count * 4u);
                uint32_t extent = 0u;
                for (uint32_t n = 0; n < count; ++n) if (offsets[n] + 12u > extent) extent = offsets[n] + 12u;
                const float *pa = (const float *)(const void *)recomp_memory(load_u(a + 4u * normals), (size_t)count * 12u);
                const float *pb = kind == RECOMP_MORPH_ONE ? NULL :
                    (const float *)(const void *)recomp_memory(load_u(b + 4u * normals), (size_t)count * 12u);
                const float *pc = kind != RECOMP_MORPH_THREE ? NULL :
                    (const float *)(const void *)recomp_memory(load_u(c + 4u * normals), (size_t)count * 12u);
                recomp_morph_blend(recomp_memory(buffer, extent), extent, count, offsets, kind, variant == 1u,
                    pa, wa, pb, wb, pc, wc);
            }
        }
    }
    recomp_runtime.registers.esp += 4u;
}

/* RECOMP_BODY_MORPH_VERIFY=1 (a check, not for play): each record is
   blended by the generated code, then from the same state by this file's,
   and everything the blend writes is compared: the record, the locked
   buffer word and every vertex it touches, and the lock's own result (the
   same resource locked, the same buffer returned). Not compared: the push
   buffer the lock writes to. The lock is library code that may first kick
   the push buffer and wait on a GPU fence (0x001EA050); the second lock of
   a check finds the fence the first one left and skips the kick, so the
   push buffer's write pointer can differ by that kick. */
typedef struct Region { uint32_t address, size; } Region;

static size_t regions(uint32_t record, Region *out, size_t limit)
{
    size_t n = 0;
    out[n++] = (Region){record, 0x24u};
    out[n++] = (Region){LOCKED_BUFFER, 4u};
    const uint32_t model = load_u(record + 0x04u), set = load_u(record);
    if (load_b(record + 0x22u) == 0u || model == 0u || set == 0u) return n;
    const uint32_t resource = load_u(model + 0x24u + (uint32_t)load_b(record + 0x20u) * 16u);
    const uint32_t buffer = load_u(resource + 4u) & ADDRESS_MASK;
    for (unsigned normals = 0; normals < 2u; ++normals) {
        const uint32_t count = load_u(set + 0x08u + 4u * normals);
        if ((int32_t)count <= 0) continue;
        const uint32_t offsets = load_u(set + 0x14u + 4u * normals);
        for (uint32_t i = 0; i < count && n < limit; ++i) out[n++] = (Region){buffer + load_u(offsets + 4u * i), 12u};
    }
    return n;
}

static void verified(void)
{
    static uint64_t calls, differing;
    enum { LIMIT = 8192 };
    static Region list[LIMIT];
    const uint32_t record = recomp_runtime.registers.esi;
    const size_t count = regions(record, list, LIMIT);
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) total += list[i].size;
    uint8_t *before = malloc(total), *theirs = malloc(total), *mine = malloc(total);
    if (before == NULL || theirs == NULL || mine == NULL) {
        free(before); free(theirs); free(mine);
        blend_record();
        return;
    }
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) { memcpy(before + at, recomp_memory(list[i].address, list[i].size), list[i].size); at += list[i].size; }
    const RecompRegisters entry = recomp_runtime.registers;
    RecompFpuContext fpu;
    recomp_fpu_context_save(&fpu);

    sub_00064FD0();
    const uint32_t esp_theirs = recomp_runtime.registers.esp;
    /* The resource the game locked, if it locked (it stores it first; the
       lock returns the resource's +4 with bit 31 set). */
    const uint32_t resource_theirs = load_u(LOCKED_BUFFER);
    at = 0;
    for (size_t i = 0; i < count; ++i) { memcpy(theirs + at, recomp_memory(list[i].address, list[i].size), list[i].size); at += list[i].size; }

    at = 0;
    for (size_t i = 0; i < count; ++i) { memcpy(recomp_memory(list[i].address, list[i].size), before + at, list[i].size); at += list[i].size; }
    recomp_runtime.registers = entry;
    recomp_fpu_context_restore(&fpu);

    last_locked_resource = last_locked_buffer = 0u;
    blend_record();
    at = 0;
    for (size_t i = 0; i < count; ++i) { memcpy(mine + at, recomp_memory(list[i].address, list[i].size), list[i].size); at += list[i].size; }

    ++calls;
    bool same = esp_theirs == recomp_runtime.registers.esp;
    if (last_locked_resource != 0u &&
        (last_locked_resource != resource_theirs || last_locked_buffer != (load_u(resource_theirs + 4u) | 0x80000000u))) {
        same = false;
        fprintf(stderr, "body morph verify: call %llu lock differs: resource %08x buffer %08x, game resource %08x\n",
            (unsigned long long)calls + 1u, last_locked_resource, last_locked_buffer, resource_theirs);
    }
    at = 0;
    for (size_t i = 0; i < count; ++i) {
        for (uint32_t k = 0; k + 4u <= list[i].size; k += 4u) {
            if (memcmp(theirs + at + k, mine + at + k, 4u) == 0) continue;
            same = false;
            if (differing < 40u) {
                uint32_t x = 0, y = 0;
                memcpy(&x, theirs + at + k, 4u);
                memcpy(&y, mine + at + k, 4u);
                fprintf(stderr, "body morph verify: call %llu record 0x%08x differs at 0x%08x: game %08x mine %08x\n",
                    (unsigned long long)calls, record, list[i].address + k, x, y);
            }
        }
        at += list[i].size;
    }
    if (!same) ++differing;
    if (calls % 2000u == 0u || !same)
        fprintf(stderr, "body morph verify: %llu calls, %llu differing (%zu regions last call)\n",
            (unsigned long long)calls, (unsigned long long)differing, count);
    free(before); free(theirs); free(mine);
}

static void blend_record_entry(void)
{
    static int verify = -1;
    if (verify < 0) verify = getenv("RECOMP_BODY_MORPH_VERIFY") != NULL;
    if (verify) verified();
    else blend_record();
}

RecompFunction recomp_body_morph_lookup_manual(uint32_t guest_address)
{
    /* Seam: the game's blend of one morph record (manual call target in the
       recipe); the D3D vertex buffer lock it calls stays the library's.
       docs/body-morph.md. */
    return guest_address == BLEND_RECORD ? blend_record_entry : NULL;
}
#else
RecompFunction recomp_body_morph_lookup_manual(uint32_t guest_address)
{
    (void)guest_address;
    return NULL;
}
#endif
