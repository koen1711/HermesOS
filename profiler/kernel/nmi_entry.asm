.code64

.extern profiler_nmi_handler
.extern exception_2

.section .text

.global profiler_nmi_entry
profiler_nmi_entry:
    pushq $0            # error code
    pushq $2            # vector

    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld
    movq %rsp, %rdi
    leaq 176(%rsp), %rsi    # past the 22-word frame: the per-CPU slot
    call profiler_nmi_handler
    movq %rax, 120(%rsp)    # the vector's slot

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    # 1 is NMI_CLAIMED, 0 is NMI_UNCLAIMED_ON_IST, else a kernel stack holding the iret frame
    cmpq $1, (%rsp)
    je 1f
    cmpq $0, (%rsp)
    je 2f
    movq (%rsp), %rsp
    jmp exception_2
1:
    leaq 16(%rsp), %rsp     # vector and error code
    iretq
2:
    leaq 16(%rsp), %rsp
    jmp exception_2         # from kernel mode: handle_exception halts, it doesn't return
