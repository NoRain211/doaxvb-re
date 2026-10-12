/* Native fiber switch for POSIX x86-64 (System V) hosts. recomp_fiber_switch
   saves the callee-saved registers on the current stack, stores that stack
   pointer in *from_sp, and resumes the fiber whose saved stack pointer is
   to_sp. A new fiber's frame (built by CreateFiberEx in fiber_adapter.c)
   returns into recomp_fiber_trampoline with r12 = parameter and r13 = entry. */
#if defined(__x86_64__)

#if defined(__APPLE__)
#define CSYM(name) _##name
#else
#define CSYM(name) name
#endif

.text
.globl CSYM(recomp_fiber_switch)
.p2align 4
CSYM(recomp_fiber_switch):
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
.p2align 4
CSYM(recomp_fiber_trampoline):
    movq  %r12, %rdi
    andq  $-16, %rsp
    callq *%r13
    ud2
#endif

#if defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif
