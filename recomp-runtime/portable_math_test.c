/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "portable_math.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Every host must give these exact bits; run on x86-64 Windows, Linux and arm64. */
static const uint64_t expected_hash = 0x946a6099083f79e7u;

static uint64_t bits(double value)
{
    uint64_t u;
    if (isnan(value)) return 0x7ff8000000000000u; /* x86 and Arm make NaNs of opposite sign */
    memcpy(&u, &value, sizeof u);
    return u;
}

static uint64_t hash = 0xcbf29ce484222325u;
static void mix(double value)
{
    uint64_t u = bits(value);
    for (int i = 0; i < 8; ++i) {
        hash ^= (u >> (8 * i)) & 0xffu;
        hash *= 0x100000001b3u;
    }
}

/* Units in the last place between two finite results of one sign. */
static uint64_t ulps(double a, double b)
{
    if (isnan(a) || isnan(b)) return isnan(a) && isnan(b) ? 0u : UINT64_MAX;
    if (a == b) return 0u;
    if ((a < 0) != (b < 0) || isinf(a) || isinf(b)) return UINT64_MAX;
    const uint64_t x = bits(fabs(a)), y = bits(fabs(b));
    return x > y ? x - y : y - x;
}

static uint64_t state = 0x9e3779b97f4a7c15u;
static uint64_t next(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

/* Uniform in [-12, 12], or a random sign and a magnitude spread over 2^-40..2^67.
   Built only from correctly rounded IEEE basic operations, so every host tests the same inputs. */
static double sample(int i)
{
    const double unit = (double)(next() >> 11) / 9007199254740992.0;
    if (i % 2 == 0) return (unit * 2.0 - 1.0) * 12.0;
    const double magnitude = ldexp(1.0 + unit, (int)(next() % 107u) - 40);
    return (next() & 1u) ? -magnitude : magnitude;
}

typedef double (*Unary)(double);
static int failures;

static void check_unary(const char *name, Unary ours, Unary host, const double *specials, size_t count, double min)
{
    unsigned differ = 0u, total = 0u;
    for (size_t i = 0; i < count + 20000u; ++i) {
        double x = i < count ? specials[i] : sample((int)i);
        if (i >= count && x < min) x = min - x; /* keep log2 in its domain */
        const double got = ours(x), want = host(x);
        mix(got);
        ++total;
        if (bits(got) != bits(want)) ++differ;
        if (ulps(got, want) > 2u) {
            if (failures < 20)
                fprintf(stderr, "FAIL %s(%.17g) = %.17g, host %.17g\n", name, x, got, want);
            ++failures;
        }
    }
    printf("%-6s differs from this host's libm in the last bits for %u of %u inputs\n", name, differ, total);
}

static double host_sin(double x) { return sin(x); }
static double host_cos(double x) { return cos(x); }
static double host_tan(double x) { return tan(x); }
static double host_atan(double x) { return atan(x); }
static double host_log2(double x) { return log2(x); }
static double host_expm1(double x) { return expm1(x); }

int main(void)
{
    const double inf = INFINITY;
    const double trig[] = {0.0, -0.0, 1.0, -1.0, 0.5, 1.5707963267948966, 3.141592653589793, 4.71238898038469,
        6.283185307179586, 1e-300, 5e-324, 1e6, 1.5e6, 1e9, 1e15, 1e22, 1e300, DBL_MAX, 0x1p62, -0x1p62,
        inf, -inf, NAN};
    const double any[] = {0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 10.0, -10.0, 700.0, 710.0, -745.0, 1e-300, 5e-324,
        DBL_MIN, DBL_MAX, -DBL_MAX, inf, -inf, NAN};
    const double logs[] = {0.0, -0.0, 1.0, 2.0, 0.5, 3.0, 1024.0, 1e-300, 5e-324, DBL_MIN, DBL_MAX, inf, -1.0,
        -0.5, -0.999999, NAN};
    check_unary("sin", recomp_sin, host_sin, trig, sizeof trig / sizeof *trig, -inf);
    check_unary("cos", recomp_cos, host_cos, trig, sizeof trig / sizeof *trig, -inf);
    check_unary("tan", recomp_tan, host_tan, trig, sizeof trig / sizeof *trig, -inf);
    check_unary("atan", recomp_atan, host_atan, any, sizeof any / sizeof *any, -inf);
    check_unary("log2", recomp_log2, host_log2, logs, sizeof logs / sizeof *logs, 0.0);
    check_unary("expm1", recomp_expm1, host_expm1, any, sizeof any / sizeof *any, -inf);

    unsigned differ = 0u;
    for (int i = 0; i < 20000; ++i) {
        const double y = sample(i), x = (i % 5 == 0) ? 0.0 : sample(i + 1);
        const double got = recomp_atan2(y, x), want = atan2(y, x);
        mix(got);
        if (bits(got) != bits(want)) ++differ;
        if (ulps(got, want) > 2u) {
            if (failures < 20) fprintf(stderr, "FAIL atan2(%.17g, %.17g) = %.17g, host %.17g\n", y, x, got, want);
            ++failures;
        }
    }
    printf("atan2  differs from this host's libm in the last bits for %u of 20000 inputs\n", differ);

    /* fpatan's quadrants, signed zeros, infinities and NaN: C Annex F fixes these results exactly. */
    const double edges[] = {0.0, -0.0, 1.0, -1.0, inf, -inf, NAN};
    for (size_t i = 0; i < sizeof edges / sizeof *edges; ++i) {
        for (size_t j = 0; j < sizeof edges / sizeof *edges; ++j) {
            const double y = edges[i], x = edges[j];
            const double got = recomp_atan2(y, x), want = atan2(y, x);
            if (bits(got) != bits(want)) {
                fprintf(stderr, "FAIL atan2(%g, %g) = %.17g, host %.17g\n", y, x, got, want);
                ++failures;
            }
        }
    }

    printf("hash %016" PRIx64 "\n", hash);
    if (hash != expected_hash) {
        fprintf(stderr, "FAIL hash %016" PRIx64 ", expected %016" PRIx64 "\n", hash, expected_hash);
        ++failures;
    }
    if (failures == 0) puts("portable math tests passed");
    return failures == 0 ? 0 : 1;
}
