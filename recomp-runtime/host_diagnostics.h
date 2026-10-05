#ifndef DOAXBV_RECOMP_HOST_DIAGNOSTICS_H
#define DOAXBV_RECOMP_HOST_DIAGNOSTICS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Windows unhandled exceptions and POSIX fatal signals emit host-crash events.
   POSIX unwinding is best effort; fatal signals bypass runtime-stop expectations.
   Debug additionally logs observed exceptions (terminal=unknown) and continuing
   CRT runtime checks (terminal=false); neither alone establishes a crash. */
void recomp_install_host_diagnostics(void);

#ifdef __cplusplus
}
#endif

#endif
