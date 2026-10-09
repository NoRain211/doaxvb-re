#ifndef DOAXBV_RECOMP_TYPES_H
#define DOAXBV_RECOMP_TYPES_H

#define RECOMP_RUNTIME_NO_GENERATED_MACROS
#include "runtime.h"
#undef RECOMP_RUNTIME_NO_GENERATED_MACROS

#include <math.h>
#include <string.h>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

static inline uint32_t BSWAP32(uint32_t value)
{
    return (value >> 24) | ((value >> 8) & 0x0000ff00u) |
           ((value << 8) & 0x00ff0000u) | (value << 24);
}

/* Generated INT3 sites report their guest site instead of trapping the host. */
#undef __debugbreak
#define __debugbreak() recomp_generated_breakpoint(__FILE__, __LINE__)

typedef RecompFunction recomp_func_t;

recomp_func_t recomp_lookup(uint32_t guest_address);
recomp_func_t recomp_lookup_manual(uint32_t guest_address);
recomp_func_t recomp_lookup_kernel(uint32_t guest_address);

extern uint32_t g_recomp_178bb0_origin;
extern uint32_t g_recomp_186fb5_origin;
extern uint32_t g_recomp_17f8e0_origin;
extern uint32_t g_recomp_78d80_origin;
extern uint32_t g_recomp_7c450_origin;
extern uint32_t g_recomp_7c7c0_origin;

#define eax recomp_runtime.registers.eax
#define ecx recomp_runtime.registers.ecx
#define edx recomp_runtime.registers.edx
#define ebx recomp_runtime.registers.ebx
#define esi recomp_runtime.registers.esi
#define edi recomp_runtime.registers.edi
#define esp recomp_runtime.registers.esp
#define g_esp (*recomp_esp_register())
#define g_seh_ebp recomp_runtime.registers.ebp
/* The lifter publishes the established frame as g_ebp and re-publishes it
   before calls; g_seh_ebp is the same guest EBP the SEH helper path reads.
   One register, two emitted names. */
#define g_ebp recomp_runtime.registers.ebp
#define g_fp_stack recomp_runtime.fpu_stack
#define g_fp_top recomp_runtime.fpu_top
#define g_fp_control_word recomp_runtime.fpu_control_word
#define g_fp_cmp recomp_runtime.fpu_compare

/* Result of an x87 compare, in the shape the status word wants:
 *   -1 less, 0 equal, 1 greater, 2 unordered (either operand is NaN).
 * The unordered case matters: `fucompp` of a value with itself followed by
 * `test ah, 0x44; jp` is how this era's CRT asks "is this a NaN", and
 * collapsing it to "equal" answers no every time.
 * Ported from the lifter's own runtime template. */
#define RECOMP_FCMP(a, b) \
    (((a) != (a) || (b) != (b)) ? 2 : (a) < (b) ? -1 : (a) > (b) ? 1 : 0)

/* x86 parity flag: 1 when the low byte of the result has an EVEN number of set
 * bits. Used by the x87 float-compare idiom `fnstsw ax; test ah, mask; jp/jnp`,
 * which is how all pre-SSE code branches on a float comparison.
 * Ported from the lifter's own runtime template. */
static inline int recomp_parity8(uint32_t x)
{
    x &= 0xFFu; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return (int)(~x & 1u);   /* 1 = even parity (PF set) */
}
#define RECOMP_PARITY8(x) recomp_parity8((uint32_t)(x))

#define MEM32(address) (*recomp_memory_u32((uint32_t)(address)))
#define MEM16(address) (*recomp_memory_u16((uint32_t)(address)))
#define MEM8(address) \
    (*(uint8_t *)(void *)recomp_memory_i8((uint32_t)(address)))
#define SMEM32(address) \
    (*(int32_t *)(void *)recomp_memory_u32((uint32_t)(address)))
#define SMEM64(address) \
    (*(int64_t *)(void *)recomp_memory_u64((uint32_t)(address)))
#define SMEM16(address) \
    (*(int16_t *)(void *)recomp_memory_u16((uint32_t)(address)))
#define SMEM8(address) (*recomp_memory_i8((uint32_t)(address)))
#define MEMF(address) \
    (*(float *)(void *)recomp_memory_u32((uint32_t)(address)))
#define MEMD(address) \
    (*(double *)(void *)recomp_memory_u64((uint32_t)(address)))
#define XBOX_PTR(address) ((void *)(uintptr_t)(uint32_t)(address))
#undef memcpy
#define memcpy(destination, source, size) \
    recomp_guest_memcpy( \
        (uint32_t)(uintptr_t)(destination), \
        (uint32_t)(uintptr_t)(source), \
        (size_t)(size))
#undef memset
#define memset(destination, value, size) \
    recomp_guest_memset( \
        (uint32_t)(uintptr_t)(destination), \
        (int)(value), \
        (size_t)(size))

#define ZX8(value) ((uint32_t)(uint8_t)(value))
#define ZX16(value) ((uint32_t)(uint16_t)(value))
#define SX8(value) ((uint32_t)(int32_t)(int8_t)(uint8_t)(value))
#define SX16(value) ((uint32_t)(int32_t)(int16_t)(uint16_t)(value))
#define LO8(value) ((uint8_t)((value) & 0xffu))
#define HI8(value) ((uint8_t)(((value) >> 8) & 0xffu))
/* x86 PF: set when the low byte of the result has an even number of set
   bits. The CRT branches on it after FNSTSW AX; TEST AH, mask. */
#define PARITY8(value) \
    ((((0x9669u >> (((uint8_t)(value) ^ ((uint8_t)(value) >> 4)) & 0xfu)) \
       & 1u)) != 0u)
#define LO16(value) ((uint16_t)((value) & 0xffffu))
#define SET_HI8(value, high) \
    ((value) = ((value) & 0xffff00ffu) | \
               ((uint32_t)(uint8_t)(high) << 8))
#define SET_LO8(value, low) \
    ((value) = ((value) & 0xffffff00u) | (uint32_t)(uint8_t)(low))
#define SET_LO16(value, low) \
    ((value) = ((value) & 0xffff0000u) | ((uint32_t)(uint16_t)(low)))

#define CMP_EQ(a, b) ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a, b) ((uint32_t)(a) != (uint32_t)(b))
#define CMP_B(a, b) ((uint32_t)(a) < (uint32_t)(b))
#define CMP_AE(a, b) ((uint32_t)(a) >= (uint32_t)(b))
#define CMP_BE(a, b) ((uint32_t)(a) <= (uint32_t)(b))
#define CMP_A(a, b) ((uint32_t)(a) > (uint32_t)(b))
/* x86 takes SF from the top bit of the operand width, not from bit 31. The
   generated code hands LO8/HI8/LO16 sub-register reads straight to these
   macros, so recover the operand width and sign-extend at that width. A
   sub-register read zero-extends, so casting it to int32_t would make every
   signed test read as non-negative. */
#define RECOMP_FLAG_WIDTH(a, b) (sizeof(a) < sizeof(b) ? sizeof(a) : sizeof(b))
#define RECOMP_SIGNED(value, width) \
    ((width) == 1u ? (int32_t)(int8_t)(uint8_t)(uint32_t)(value) \
     : (width) == 2u ? (int32_t)(int16_t)(uint16_t)(uint32_t)(value) \
     : (int32_t)(uint32_t)(value))
#define CMP_L(a, b) \
    (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) < \
     RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))
#define CMP_GE(a, b) \
    (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >= \
     RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))
#define CMP_LE(a, b) \
    (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <= \
     RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))
#define CMP_G(a, b) \
    (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) > \
     RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))
#define TEST_Z(a, b) (((uint32_t)(a) & (uint32_t)(b)) == 0u)
#define TEST_NZ(a, b) (((uint32_t)(a) & (uint32_t)(b)) != 0u)
#define TEST_S(a, b) \
    (RECOMP_SIGNED((uint32_t)(a) & (uint32_t)(b), \
                   RECOMP_FLAG_WIDTH(a, b)) < 0)

static inline uint32_t ROL32(uint32_t value, unsigned int count)
{
    count &= 31u;
    return count == 0u ? value : (value << count) | (value >> (32u - count));
}

static inline uint32_t ROR32(uint32_t value, unsigned int count)
{
    count &= 31u;
    return count == 0u ? value : (value >> count) | (value << (32u - count));
}

#define PUSH32(sp, value) do { \
    uint32_t push_value = (uint32_t)(value); \
    (sp) -= 4u; \
    MEM32(sp) = push_value; \
} while (0)
#define POP32(sp, value) do { \
    (value) = MEM32(sp); \
    (sp) += 4u; \
} while (0)
#define RECOMP_ICALL_SAFE(address, saved_esp) do { \
    recomp_dispatch_indirect_site( \
        (uint32_t)(address), (uint32_t)(saved_esp), __FILE__, __LINE__); \
} while (0)
#define RECOMP_ITAIL(address) do { \
    recomp_dispatch_indirect_site( \
        (uint32_t)(address), g_esp, __FILE__, __LINE__); \
} while (0)

/* ── SSE ────────────────────────────────────────────────────────────────
   RecompXmm and the register file live in runtime.h beside the other
   architectural state. These names alias it so generated code can keep
   writing xmm0..xmm7 directly. */
#define xmm0 recomp_runtime.xmm[0]
#define xmm1 recomp_runtime.xmm[1]
#define xmm2 recomp_runtime.xmm[2]
#define xmm3 recomp_runtime.xmm[3]
#define xmm4 recomp_runtime.xmm[4]
#define xmm5 recomp_runtime.xmm[5]
#define xmm6 recomp_runtime.xmm[6]
#define xmm7 recomp_runtime.xmm[7]

/* Packed transfers keep the guest side under the normal address translation,
   so bounds checks, the cached alias, and the APU aperture still apply. The
   register itself is a host local and is addressed as a host pointer. */
#define XMM_MEM(address) recomp_xmm_mem((uint32_t)(address))
#define XMM_STORE(address, reg) \
    recomp_guest_store((uint32_t)(address), &(reg), 16u)
#define XMM_LOAD_LOW(reg, address) \
    recomp_guest_load(&(reg).f[0], (uint32_t)(address), 8u)
#define XMM_STORE_LOW(address, reg) \
    recomp_guest_store((uint32_t)(address), &(reg).f[0], 8u)
#define XMM_LOAD_HIGH(reg, address) \
    recomp_guest_load(&(reg).f[2], (uint32_t)(address), 8u)
#define XMM_STORE_HIGH(address, reg) \
    recomp_guest_store((uint32_t)(address), &(reg).f[2], 8u)

static inline RecompXmm recomp_xmm_mem(uint32_t address)
{
    RecompXmm value;

    recomp_guest_load(&value, address, 16u);
    return value;
}

static inline RecompXmm XMM_ZERO(void)
{
    RecompXmm value;

    value.u[0] = 0u;
    value.u[1] = 0u;
    value.u[2] = 0u;
    value.u[3] = 0u;
    return value;
}

/* MOVSS from memory zeroes bits 127:32; MOVSS between registers does not. */
static inline RecompXmm XMM_SCALAR(float lane0)
{
    RecompXmm value = XMM_ZERO();

    value.f[0] = lane0;
    return value;
}

static inline RecompXmm XMM_SCALAR_BITS(uint32_t lane0)
{
    RecompXmm value = XMM_ZERO();

    value.u[0] = lane0;
    return value;
}

static inline RecompXmm XMM_SCALAR_DOUBLE(double lane0)
{
    RecompXmm value = XMM_ZERO();

    value.d[0] = lane0;
    return value;
}

#define RECOMP_XMM_LANEWISE(name, expression) \
    static inline RecompXmm name(RecompXmm a, RecompXmm b) \
    { \
        RecompXmm r; \
        int i; \
        for (i = 0; i < 4; ++i) { \
            expression; \
        } \
        return r; \
    }

RECOMP_XMM_LANEWISE(XMM_ADD, r.f[i] = a.f[i] + b.f[i])
RECOMP_XMM_LANEWISE(XMM_SUB, r.f[i] = a.f[i] - b.f[i])
RECOMP_XMM_LANEWISE(XMM_MUL, r.f[i] = a.f[i] * b.f[i])
RECOMP_XMM_LANEWISE(XMM_DIV, r.f[i] = a.f[i] / b.f[i])
/* MINPS/MAXPS return the second operand when either input is NaN. */
RECOMP_XMM_LANEWISE(XMM_MIN, r.f[i] = a.f[i] < b.f[i] ? a.f[i] : b.f[i])
RECOMP_XMM_LANEWISE(XMM_MAX, r.f[i] = a.f[i] > b.f[i] ? a.f[i] : b.f[i])
RECOMP_XMM_LANEWISE(XMM_AND, r.u[i] = a.u[i] & b.u[i])
RECOMP_XMM_LANEWISE(XMM_ANDN, r.u[i] = ~a.u[i] & b.u[i])
RECOMP_XMM_LANEWISE(XMM_OR, r.u[i] = a.u[i] | b.u[i])
RECOMP_XMM_LANEWISE(XMM_XOR, r.u[i] = a.u[i] ^ b.u[i])
RECOMP_XMM_LANEWISE(XMM_CMP_EQ, r.u[i] = a.f[i] == b.f[i] ? 0xffffffffu : 0u)
RECOMP_XMM_LANEWISE(XMM_CMP_LT, r.u[i] = a.f[i] < b.f[i] ? 0xffffffffu : 0u)
RECOMP_XMM_LANEWISE(XMM_CMP_LE, r.u[i] = a.f[i] <= b.f[i] ? 0xffffffffu : 0u)
RECOMP_XMM_LANEWISE(XMM_CMP_NEQ, r.u[i] = a.f[i] != b.f[i] ? 0xffffffffu : 0u)

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
static inline RecompXmm recomp_xmm_from_native(__m128 value)
{
    RecompXmm r;

    _mm_storeu_ps(r.f, value);
    return r;
}

/* SHUFPS carries an immediate byte; retain it for the native instruction. */
#define XMM_SHUFFLE(a, b, imm) \
    recomp_xmm_from_native(_mm_shuffle_ps( \
        _mm_loadu_ps((a).f), _mm_loadu_ps((b).f), (imm)))
#else
static inline RecompXmm recomp_xmm_shuffle(RecompXmm a, RecompXmm b, unsigned int imm)
{
    RecompXmm r;
    r.u[0] = a.u[(imm >> 0) & 3u];
    r.u[1] = a.u[(imm >> 2) & 3u];
    r.u[2] = b.u[(imm >> 4) & 3u];
    r.u[3] = b.u[(imm >> 6) & 3u];
    return r;
}
#define XMM_SHUFFLE(a, b, imm) recomp_xmm_shuffle((a), (b), (imm))
#endif

static inline RecompXmm XMM_UNPACK_LOW(RecompXmm a, RecompXmm b)
{
    RecompXmm r;

    r.u[0] = a.u[0];
    r.u[1] = b.u[0];
    r.u[2] = a.u[1];
    r.u[3] = b.u[1];
    return r;
}

static inline RecompXmm XMM_UNPACK_HIGH(RecompXmm a, RecompXmm b)
{
    RecompXmm r;

    r.u[0] = a.u[2];
    r.u[1] = b.u[2];
    r.u[2] = a.u[3];
    r.u[3] = b.u[3];
    return r;
}

/* MOVLHPS: destination high half takes the source low half. */
static inline RecompXmm XMM_MOVE_LOW_TO_HIGH(RecompXmm a, RecompXmm b)
{
    RecompXmm r = a;

    r.u[2] = b.u[0];
    r.u[3] = b.u[1];
    return r;
}

/* MOVHLPS: destination low half takes the source high half. */
static inline RecompXmm XMM_MOVE_HIGH_TO_LOW(RecompXmm a, RecompXmm b)
{
    RecompXmm r = a;

    r.u[0] = b.u[2];
    r.u[1] = b.u[3];
    return r;
}

static inline uint32_t XMM_MOVEMASK(RecompXmm a)
{
    return ((a.u[0] >> 31) & 1u) | ((a.u[1] >> 30) & 2u) |
           ((a.u[2] >> 29) & 4u) | ((a.u[3] >> 28) & 8u);
}

/* ponytail: use host MXCSR rounding; model guest MXCSR when a lifted caller
   writes it. SSE conversions avoid disturbing the host x87/MMX register file. */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
static inline uint64_t MMX_CVTPS2PI(float low, float high)
{
    uint32_t a = (uint32_t)_mm_cvtss_si32(_mm_set_ss(low));
    uint32_t b = (uint32_t)_mm_cvtss_si32(_mm_set_ss(high));
    return a | ((uint64_t)b << 32);
}
#else
static inline uint32_t recomp_cvtss_si32(float val)
{
    if (isnan(val) || isinf(val)) return 0x80000000u;
    if (val >= 2147483584.0f || val < -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)lrintf(val);
}

static inline uint64_t MMX_CVTPS2PI(float low, float high)
{
    uint32_t a = recomp_cvtss_si32(low);
    uint32_t b = recomp_cvtss_si32(high);
    return a | ((uint64_t)b << 32);
}
#endif

static inline uint64_t MMX_PACKSSDW(uint64_t a, uint64_t b)
{
    uint64_t packed = 0u;
    for (unsigned lane = 0u; lane < 4u; ++lane) {
        uint64_t source = lane < 2u ? a : b;
        int32_t value = (int32_t)(uint32_t)(source >> ((lane & 1u) * 32u));
        if (value < -32768) value = -32768;
        if (value > 32767) value = 32767;
        packed |= (uint64_t)(uint16_t)value << (lane * 16u);
    }
    return packed;
}

static inline uint64_t MMX_PAVGB(uint64_t a, uint64_t b)
{
    uint64_t packed = 0u;
    for (unsigned shift = 0u; shift < 64u; shift += 8u) {
        unsigned sum = (unsigned)((a >> shift) & 255u) +
            (unsigned)((b >> shift) & 255u) + 1u;
        packed |= (uint64_t)(sum >> 1u) << shift;
    }
    return packed;
}

static inline uint64_t MMX_MEM(uint32_t address)
{
    uint64_t value;
    recomp_guest_load(&value, address, 8u);
    return value;
}

#define MMX_STORE(address, reg) \
    recomp_guest_store((uint32_t)(address), &(reg), 8u)

#endif
