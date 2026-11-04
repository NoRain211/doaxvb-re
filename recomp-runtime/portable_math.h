/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOAXBV_RECOMP_PORTABLE_MATH_H
#define DOAXBV_RECOMP_PORTABLE_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

/* The x87 transcendentals generated code calls (through recomp_types.h):
   musl's (musl_math/), built the same on every host, so two machines in a
   netplay session compute the same bits where the hosts' own libm differ in
   the last one. */
double recomp_sin(double x);
double recomp_cos(double x);
double recomp_tan(double x);
double recomp_atan(double x);
double recomp_atan2(double y, double x);
double recomp_log2(double x);
double recomp_expm1(double x);

#ifdef __cplusplus
}
#endif

#endif
