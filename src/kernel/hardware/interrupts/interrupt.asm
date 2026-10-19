.code64
.equ ENOSYS, 38

.global irq_interrupt_syscall

.extern handle_pic_interrupt
.extern handle_syscall

irq_interrupt_syscall:
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

    # Push error code or syscall number
    pushq $-ENOSYS  # Use immediate value with pushq

    # Call the syscall handler
    # In 64-bit calling convention, arguments are passed in registers
    # You might need to set up arguments in %rdi, %rsi, etc.
    call handle_syscall

    # Clean up the pushed error code
    addq $8, %rsp   # 8 bytes in 64-bit mode

    # Restore all registers in reverse order
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

    iretq

.macro IRQ_STUB n
.global irq_interrupt_\n
irq_interrupt_\n:
    pushq $\n
    jmp irq_interrupt
.endm

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

irq_interrupt:
    sub $160, %rsp

    mov %rax, 0(%rsp)
    mov %rbx, 8(%rsp)
    mov %rcx, 16(%rsp)
    mov %rdx, 24(%rsp)
    mov %rbp, 32(%rsp)
    mov %rsi, 48(%rsp)
    mov %rdi, 56(%rsp)
    mov %r8, 64(%rsp)
    mov %r9, 72(%rsp)
    mov %r10, 80(%rsp)
    mov %r11, 88(%rsp)
    mov %r12, 96(%rsp)
    mov %r13, 104(%rsp)
    mov %r14, 112(%rsp)
    mov %r15, 120(%rsp)

    mov 160(%rsp), %rax     # irq number pushed by the stub
    mov %rax, 152(%rsp)
    mov 168(%rsp), %rax     # rip
    mov %rax, 128(%rsp)
    mov 176(%rsp), %rax     # cs
    mov %rax, 136(%rsp)
    mov 184(%rsp), %rax     # rflags
    mov %rax, 144(%rsp)
    mov 192(%rsp), %rax     # interrupted rsp
    mov %rax, 40(%rsp)

    mov %rsp, %rdi          # interrupt_context*
    cld
    call handle_pic_interrupt

    mov 0(%rsp), %rax
    mov 8(%rsp), %rbx
    mov 16(%rsp), %rcx
    mov 24(%rsp), %rdx
    mov 32(%rsp), %rbp
    mov 48(%rsp), %rsi
    mov 56(%rsp), %rdi
    mov 64(%rsp), %r8
    mov 72(%rsp), %r9
    mov 80(%rsp), %r10
    mov 88(%rsp), %r11
    mov 96(%rsp), %r12
    mov 104(%rsp), %r13
    mov 112(%rsp), %r14
    mov 120(%rsp), %r15

    add $168, %rsp          # context + irq number
    iretq
