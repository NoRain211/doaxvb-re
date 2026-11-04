/* Altered for doa2u-re from musl 1.2.5 src/internal/libm.h (MIT, see
   COPYRIGHT): only what these files use, with musl's private helpers renamed
   so they never meet the host C library's. */
#ifndef DOAXBV_MUSL_LIBM_H
#define DOAXBV_MUSL_LIBM_H

#include "../portable_math.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define hidden
#define predict_false(x) (x)
#define __rem_pio2_large recomp_musl_rem_pio2_large
#define __rem_pio2 recomp_musl_rem_pio2
#define __sin recomp_musl_sin
#define __cos recomp_musl_cos
#define __tan recomp_musl_tan

static inline uint64_t asuint64(double f)
{
	uint64_t i;
	memcpy(&i, &f, sizeof i);
	return i;
}

static inline double asdouble(uint64_t i)
{
	double f;
	memcpy(&f, &i, sizeof f);
	return f;
}

static inline void fp_force_evalf(float x)
{
	volatile float y;
	y = x;
}

static inline void fp_force_eval(double x)
{
	volatile double y;
	y = x;
}

#define FORCE_EVAL(x) do {                        \
	if (sizeof(x) == sizeof(float)) {         \
		fp_force_evalf(x);                \
	} else {                                  \
		fp_force_eval(x);                 \
	}                                         \
} while(0)

#define EXTRACT_WORDS(hi,lo,d)                    \
do {                                              \
  uint64_t __u = asuint64(d);                     \
  (hi) = __u >> 32;                               \
  (lo) = (uint32_t)__u;                           \
} while (0)

#define GET_HIGH_WORD(hi,d)                       \
do {                                              \
  (hi) = asuint64(d) >> 32;                       \
} while (0)

#define GET_LOW_WORD(lo,d)                        \
do {                                              \
  (lo) = (uint32_t)asuint64(d);                   \
} while (0)

#define INSERT_WORDS(d,hi,lo)                     \
do {                                              \
  (d) = asdouble(((uint64_t)(hi)<<32) | (uint32_t)(lo)); \
} while (0)

#define SET_HIGH_WORD(d,hi)                       \
  INSERT_WORDS(d, hi, (uint32_t)asuint64(d))

#define SET_LOW_WORD(d,lo)                        \
  INSERT_WORDS(d, asuint64(d)>>32, lo)

hidden int    __rem_pio2_large(double*,double*,int,int,int);
hidden int    __rem_pio2(double,double*);
hidden double __sin(double,double,int);
hidden double __cos(double,double);
hidden double __tan(double,double,int);

#endif
