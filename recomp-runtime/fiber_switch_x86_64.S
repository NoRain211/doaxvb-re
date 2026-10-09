#if defined(__x86_64__) || defined(_M_X64)

#if defined(__APPLE__)
#define CSYM(name) _##name
#else
#define CSYM(name) name
#endif

.text
.globl CSYM(recomp_fiber_switch)
.globl _recomp_fiber_switch
.globl recomp_fiber_switch
.p2align 4
CSYM(recomp_fiber_switch):
#if defined(__APPLE__)
recomp_fiber_switch:
#else
_recomp_fiber_switch:
#endif
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, (%rdi)
    movq  %rsi, %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    ret

.globl CSYM(recomp_fiber_trampoline)
.globl _recomp_fiber_trampoline
.globl recomp_fiber_trampoline
.p2align 4
CSYM(recomp_fiber_trampoline):
#if defined(__APPLE__)
recomp_fiber_trampoline:
#else
_recomp_fiber_trampoline:
#endif
    movq  %r12, %rdi
    andq  $-16, %rsp
    callq *%r13
    ud2
#endif
