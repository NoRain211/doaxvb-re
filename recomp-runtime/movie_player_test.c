#include "movie_player.h"

#include "kernel_abi.h"
#include "program_manual.h"
#include "xapi_time_adapter.h"

#include <stdio.h>
#include <string.h>

enum {
    BASE = 0x00500000u,
    SIZE = 0x00004000u,
    ENTRY_ESP = BASE + 0x100u,
    PARAMS = BASE + 0x200u,
    WORK = BASE + 0x300u,
    NAME = BASE + 0x400u,
    FRAME = BASE + 0x500u,
    TEXTURE = BASE + 0x1000u,
    PITCH = 80u,
};

typedef struct Bits {
    uint8_t bytes[1024];
    size_t bit;
} Bits;

static void put(Bits *b, uint32_t value, unsigned count)
{
    while (count--) {
        if ((value >> count) & 1u) b->bytes[b->bit >> 3] |= (uint8_t)(0x80u >> (b->bit & 7u));
        b->bit++;
    }
}

static void start(Bits *b, uint8_t code)
{
    b->bit = (b->bit + 7u) & ~(size_t)7u;
    put(b, 0x000001u, 24);
    put(b, code, 8);
}

/* A 16x16 intra picture of luma 200 at 11-bit DC (+576), chroma 128, as in
   movie_video_test.c. */
static void picture(Bits *b)
{
    start(b, 0x00);
    put(b, 0, 10); put(b, 1, 3); put(b, 0xFFFF, 16); put(b, 0, 1);
    start(b, 0x01);
    put(b, 1, 5); put(b, 0, 1); put(b, 1, 1); put(b, 1, 1);
    put(b, 0x1FE, 9); put(b, 576, 10); put(b, 2, 2);
    for (int i = 0; i < 3; i++) { put(b, 4, 3); put(b, 2, 2); }
    for (int i = 0; i < 2; i++) { put(b, 0, 2); put(b, 2, 2); }
}

/* A program stream like an SFD file: pack, a system header that declares no
   video, then one video packet holding two pictures. */
static int write_stream(const char *path)
{
    static Bits es, ps;
    const char user_data[] = "IDCPREC\0" "0000000800000003";

    memset(&es, 0, sizeof es);
    memset(&ps, 0, sizeof ps);
    start(&es, 0xB3);
    put(&es, 16, 12); put(&es, 16, 12); put(&es, 1, 4); put(&es, 3, 4);
    put(&es, 0x3FFFF, 18); put(&es, 1, 1); put(&es, 1, 10); put(&es, 0, 3);
    start(&es, 0xB2);
    for (size_t i = 0; i < sizeof user_data - 1u; i++) put(&es, (uint8_t)user_data[i], 8);
    picture(&es);
    picture(&es);
    es.bit = ((es.bit + 7u) & ~(size_t)7u) + 160u * 8u;
    start(&es, 0xB7);
    const size_t es_bytes = es.bit >> 3;

    start(&ps, 0xBA); put(&ps, 0x21000100u, 32); put(&ps, 0x01800003u, 32);
    start(&ps, 0xBB); put(&ps, 0x00068000u, 32); put(&ps, 0x0300E0FFu, 32);
    start(&ps, 0xE0); put(&ps, (uint32_t)es_bytes + 1u, 16); put(&ps, 0x0F, 8);
    for (size_t i = 0; i < es_bytes; i++) put(&ps, es.bytes[i], 8);
    FILE *file = fopen(path, "wb");
    return file != NULL && fwrite(ps.bytes, 1, ps.bit >> 3, file) == ps.bit >> 3 && fclose(file) == 0;
}

static uint32_t call(uint32_t address, uint32_t a, uint32_t b, uint32_t c, int *passed)
{
    const uint32_t args[] = {0u, a, b, c};
    recomp_guest_store(ENTRY_ESP, args, sizeof args);
    recomp_runtime.registers.esp = ENTRY_ESP;
    RecompFunction function = recomp_lookup_manual(address);
    if (function == NULL || function != recomp_movie_player_lookup_manual(address)) {
        fprintf(stderr, "Movie player: no replacement at 0x%08x\n", address);
        *passed = 0;
        return 0u;
    }
    function();
    if (recomp_runtime.registers.esp != ENTRY_ESP + 4u) {
        fprintf(stderr, "Movie player: 0x%08x did not return cdecl\n", address);
        *passed = 0;
    }
    return recomp_runtime.registers.eax;
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "Movie player: line %d: %s\n", __LINE__, #condition); passed = 0; } } while (0)

int recomp_movie_player_test(void)
{
    static uint8_t memory[SIZE];
    const RecompMemoryRegion region = {.address = BASE, .size = sizeof memory, .data = memory};
    const char *file = "recomp_movie_player_test.sfd";
    const char *old_root = recomp_disc_root_path;
    int passed = 1;

    memset(memory, 0, sizeof memory);
    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    if (!write_stream(file)) {
        fprintf(stderr, "Movie player: cannot write %s\n", file);
        return 0;
    }
    recomp_disc_root_path = ".";
    snprintf((char *)recomp_memory(NAME, 64u), 64u, "d:\\%s", file);
    *recomp_memory_u32(PARAMS + 0x18u) = WORK;

    call(0x00199cb0u, 0u, 0u, 0u, &passed);
    const uint32_t handle = call(0x00199760u, PARAMS, 0u, 0u, &passed);
    CHECK(handle == WORK && *recomp_memory_u32(WORK) == 0x00284168u);
    CHECK(call(0x00198320u, handle, 0u, 0u, &passed) == 0u);
    CHECK(call(0x00198550u, handle, 0u, 0u, &passed) == 0u);
    call(0x001a30b0u, handle, 0u, 0u, &passed);
    call(0x001a34a0u, handle, NAME, 0u, &passed);

    /* The worker decodes the frame; it is due as soon as it is ready. */
    const uint64_t deadline = recomp_xapi_performance_counter() + 5000000000ull;
    do {
        *recomp_memory_u32(FRAME) = 0xffffffffu;
        call(0x00198190u, handle, FRAME, 0u, &passed);
    } while (*recomp_memory_u32(FRAME) == 0u && recomp_xapi_performance_counter() < deadline);
    CHECK(*recomp_memory_u32(FRAME) != 0u);
    CHECK(*recomp_memory_u32(FRAME + 8u) == 16u && *recomp_memory_u32(FRAME + 12u) == 16u);
    CHECK(call(0x00198320u, handle, 0u, 0u, &passed) == 2u);

    /* Luma 200 at studio range is 214 on every color channel. */
    memset(recomp_memory(TEXTURE, PITCH * 16u), 0xa5, PITCH * 16u);
    call(0x001985f0u, handle, PITCH, 16u, &passed);
    call(0x001989f0u, handle, FRAME, TEXTURE, &passed);
    CHECK(*recomp_memory_u32(TEXTURE) == 0xffd6d6d6u);
    CHECK(*recomp_memory_u32(TEXTURE + PITCH * 15u + 60u) == 0xffd6d6d6u);
    CHECK(*recomp_memory_u32(TEXTURE + 64u) == 0xa5a5a5a5u);
    call(0x00197ec0u, handle, 0u, 0u, &passed);

    uint32_t status;
    do {
        call(0x00198190u, handle, FRAME, 0u, &passed);
        if (*recomp_memory_u32(FRAME) != 0u) call(0x00197ec0u, handle, 0u, 0u, &passed);
        status = call(0x00198320u, handle, 0u, 0u, &passed);
    } while (status == 2u && recomp_xapi_performance_counter() < deadline);
    CHECK(status == 3u);
    call(0x001a3060u, handle, 0u, 0u, &passed);
    CHECK(call(0x00198320u, handle, 0u, 0u, &passed) == 0u);

    /* A missing file reports an error instead of stopping the game. */
    snprintf((char *)recomp_memory(NAME, 64u), 64u, "d:\\missing.sfd");
    call(0x001a34a0u, handle, NAME, 0u, &passed);
    do {
        status = call(0x00198320u, handle, 0u, 0u, &passed);
    } while (status == 1u && recomp_xapi_performance_counter() < deadline);
    CHECK(status == 4u);
    call(0x00199680u, handle, 0u, 0u, &passed);

    recomp_disc_root_path = old_root;
    remove(file);
    if (passed) puts("PASS movie player: cdecl calls, status, pacing, conversion, missing file");
    return passed;
}
