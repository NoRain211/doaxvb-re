#include "kernel_abi.h"

#include <stdio.h>
#include <string.h>

/* Expectations are the published FIPS 180-1 / RFC 3174 SHA-1 vectors plus the
   well-known digest of the empty string. They were not produced by the code
   under test, so a self-consistent but wrong implementation cannot pass. */

enum {
    TEST_MEMORY_BASE = 0x2a000000u,
    TEST_MEMORY_SIZE = 0x00001000u,
    TEST_ENTRY_ESP = TEST_MEMORY_BASE + 0x100u,
    TEST_CONTEXT = TEST_MEMORY_BASE + 0x200u,
    TEST_DIGEST = TEST_MEMORY_BASE + 0x300u,
    TEST_INPUT = TEST_MEMORY_BASE + 0x400u,
};

typedef struct ShaVector {
    const char *message;
    unsigned repeat;
    const char *digest;
} ShaVector;

static const ShaVector vectors[] = {
    {"", 1u, "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
    {"abc", 1u, "a9993e364706816aba3e25717850c26c9cd0d89d"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1u,
     "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
    /* One million 'a' is the classic long vector; 200 x 5000 keeps the host
       buffer small while still crossing thousands of block boundaries. */
    {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 15625u,
     "34aa973cd4c4daa4f61eeb2bdbad27316534016f"},
};

static int expect_digest(
    const char *label, const uint8_t *actual, const char *expected)
{
    char text[2u * RECOMP_SHA_DIGEST_BYTES + 1u];

    for (unsigned i = 0u; i < RECOMP_SHA_DIGEST_BYTES; ++i) {
        snprintf(text + 2u * i, 3u, "%02x", actual[i]);
    }
    if (strcmp(text, expected) == 0) {
        return 1;
    }
    fprintf(
        stderr, "kernel crypto: %s was %s, expected %s\n",
        label, text, expected);
    return 0;
}
static int expect_u32(const char *field, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(
        stderr, "kernel crypto: %s was 0x%08x, expected 0x%08x\n",
        field, actual, expected);
    return 0;
}

/* The model alone, with no guest memory involved. */
static int check_model(const ShaVector *vector)
{
    RecompShaContext context;
    uint8_t digest[RECOMP_SHA_DIGEST_BYTES];
    size_t length = strlen(vector->message);

    recomp_kernel_sha_init(&context);
    for (unsigned i = 0u; i < vector->repeat; ++i) {
        recomp_kernel_sha_update(
            &context, (const uint8_t *)vector->message, (uint32_t)length);
    }
    recomp_kernel_sha_final(&context, digest);
    return expect_digest("model digest", digest, vector->digest);
}

/* Feeding one byte at a time must give the same answer as one bulk update:
   that is what proves the buffered-tail path in update is right. */
static int check_byte_at_a_time(const ShaVector *vector)
{
    RecompShaContext context;
    uint8_t digest[RECOMP_SHA_DIGEST_BYTES];
    size_t length = strlen(vector->message);

    if (vector->repeat != 1u) {
        return 1;
    }
    recomp_kernel_sha_init(&context);
    for (size_t i = 0u; i < length; ++i) {
        recomp_kernel_sha_update(
            &context, (const uint8_t *)vector->message + i, 1u);
    }
    recomp_kernel_sha_final(&context, digest);
    return expect_digest("byte-at-a-time digest", digest, vector->digest);
}

int recomp_kernel_crypto_test(void);
int recomp_kernel_crypto_overrun_test(void);

/* An input span that starts in guest memory but runs past its end must stop
   the runtime on the whole span, not just its first byte. Returning means the
   host read past the buffer. */
int recomp_kernel_crypto_overrun_test(void)
{
    static uint8_t memory[TEST_MEMORY_SIZE];
    const RecompMemoryRegion region = {
        .address = TEST_MEMORY_BASE,
        .size = sizeof memory,
        .data = memory,
    };
    uint32_t *stack = (uint32_t *)(memory + TEST_ENTRY_ESP - TEST_MEMORY_BASE);

    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    stack[0] = 0x0010abcdu;
    stack[1] = TEST_CONTEXT;
    stack[2] = TEST_MEMORY_BASE + TEST_MEMORY_SIZE - 4u;
    stack[3] = 16u;
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    recomp_kernel_crypto(336u)();
    return 0;
}

int recomp_kernel_crypto_test(void)
{
    static uint8_t memory[TEST_MEMORY_SIZE];
    const RecompMemoryRegion region = {
        .address = TEST_MEMORY_BASE,
        .size = sizeof memory,
        .data = memory,
    };
    RecompFunction init = recomp_kernel_crypto(335u);
    RecompFunction update = recomp_kernel_crypto(336u);
    RecompFunction final = recomp_kernel_crypto(337u);
    uint32_t *stack;
    uint8_t digest[RECOMP_SHA_DIGEST_BYTES];
    int passed = 1;

    for (unsigned i = 0u; i < sizeof vectors / sizeof vectors[0]; ++i) {
        passed &= check_model(&vectors[i]);
        passed &= check_byte_at_a_time(&vectors[i]);
    }

    passed &= expect_u32("ordinal 335 is implemented", init != NULL, 1u);
    passed &= expect_u32("ordinal 336 is implemented", update != NULL, 1u);
    passed &= expect_u32("ordinal 337 is implemented", final != NULL, 1u);
    passed &= expect_u32(
        "ordinal 334 stays unimplemented",
        recomp_kernel_crypto(334u) != NULL, 0u);
    passed &= expect_u32(
        "ordinal 338 stays unimplemented",
        recomp_kernel_crypto(338u) != NULL, 0u);
    if (init == NULL || update == NULL || final == NULL) {
        return 0;
    }

    memset(memory, 0, sizeof memory);
    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);

    /* Drive "abc" through the three bridges the way the guest does, and check
       both the digest and the stdcall cleanup. */
    for (unsigned i = 0u; i < 3u; ++i) {
        *recomp_memory_i8(TEST_INPUT + i) = (int8_t)"abc"[i];
    }

    stack = (uint32_t *)(memory + TEST_ENTRY_ESP - TEST_MEMORY_BASE);
    stack[0] = 0x0010abcdu;
    stack[1] = TEST_CONTEXT;
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    *recomp_memory_u32(TEST_CONTEXT) = 0xfeedfaceu;
    init();
    passed &= expect_u32(
        "init ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 8u);
    /* The live state starts after the XDK context's 24-byte unused prefix,
       and that prefix must survive the call untouched. */
    passed &= expect_u32(
        "init seeds H0", *recomp_memory_u32(TEST_CONTEXT + 24u), 0x67452301u);
    passed &= expect_u32(
        "init leaves the prefix alone",
        *recomp_memory_u32(TEST_CONTEXT), 0xfeedfaceu);

    stack[0] = 0x0010abcdu;
    stack[1] = TEST_CONTEXT;
    stack[2] = TEST_INPUT;
    stack[3] = 3u;
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    update();
    passed &= expect_u32(
        "update ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 16u);

    stack[0] = 0x0010abcdu;
    stack[1] = TEST_CONTEXT;
    stack[2] = TEST_DIGEST;
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    final();
    passed &= expect_u32(
        "final ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 12u);
    passed &= expect_u32(
        "final return address survives",
        *recomp_memory_u32(TEST_ENTRY_ESP), 0x0010abcdu);
    for (unsigned i = 0u; i < RECOMP_SHA_DIGEST_BYTES; ++i) {
        digest[i] = (uint8_t)*recomp_memory_i8(TEST_DIGEST + i);
    }
    passed &= expect_digest(
        "bridge digest", digest, "a9993e364706816aba3e25717850c26c9cd0d89d");

    /* A null context is the guest's business, not a crash, and must still
       pop its arguments. */
    stack[0] = 0x0010abcdu;
    stack[1] = 0u;
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    init();
    passed &= expect_u32(
        "null context ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 8u);

    return passed;
}
