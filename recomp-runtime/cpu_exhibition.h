#ifndef DOAXBV_RECOMP_CPU_EXHIBITION_H
#define DOAXBV_RECOMP_CPU_EXHIBITION_H

#ifdef __cplusplus
extern "C" {
#endif

/* Starts Exhibition matches played by the CPU on every side, from the title
   screen. They follow one another until START is pressed. */
void recomp_cpu_exhibition_start(void);
/* Once per presented frame. */
void recomp_cpu_exhibition_frame(void);

#ifdef __cplusplus
}
#endif

#endif
