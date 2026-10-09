#if defined(__arm64__) || defined(__aarch64__)

#if defined(__APPLE__)
#define CSYM(name) _##name
#else
#define CSYM(name) name
#endif

.text
.globl CSYM(recomp_fiber_switch)
.globl _recomp_fiber_switch
.globl recomp_fiber_switch
.p2align 2
CSYM(recomp_fiber_switch):
#if defined(__APPLE__)
recomp_fiber_switch:
#else
_recomp_fiber_switch:
#endif
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x29, x30, [sp, #-16]!
    stp d8,  d9,  [sp, #-16]!
    stp d10, d11, [sp, #-16]!
    stp d12, d13, [sp, #-16]!
    stp d14, d15, [sp, #-16]!
    mov x9, sp
    str x9, [x0]
    mov sp, x1
    ldp d14, d15, [sp], #16
    ldp d12, d13, [sp], #16
    ldp d10, d11, [sp], #16
    ldp d8,  d9,  [sp], #16
    ldp x29, x30, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ret

.globl CSYM(recomp_fiber_trampoline)
.globl _recomp_fiber_trampoline
.globl recomp_fiber_trampoline
.p2align 2
CSYM(recomp_fiber_trampoline):
#if defined(__APPLE__)
recomp_fiber_trampoline:
#else
_recomp_fiber_trampoline:
#endif
    mov x0, x19
    blr x20
    brk #1
#endif
