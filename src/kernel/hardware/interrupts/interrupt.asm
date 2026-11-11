.code64

.global trap_return

.ifdef HERMES_UNIKERNEL
.set USER_ADDR_MIN, 0x40000000
.set USER_ADDR_LIMIT, 0x800000000000
.endif

.extern trap_handler
.extern syscall_dispatch

.macro TRAP_NOERR name, vector
.global \name
\name:
    pushq $0
    pushq $\vector
    jmp trap_common
.endm

.macro TRAP_ERR name, vector
.global \name
\name:
    pushq $\vector
    jmp trap_common
.endm

TRAP_NOERR exception_0,  0
TRAP_NOERR exception_1,  1
TRAP_NOERR exception_2,  2
TRAP_NOERR exception_3,  3
TRAP_NOERR exception_4,  4
TRAP_NOERR exception_5,  5
TRAP_NOERR exception_6,  6
TRAP_NOERR exception_7,  7
TRAP_ERR   exception_8,  8
TRAP_NOERR exception_9,  9
TRAP_ERR   exception_10, 10
TRAP_ERR   exception_11, 11
TRAP_ERR   exception_12, 12
TRAP_ERR   exception_13, 13
TRAP_ERR   exception_14, 14
TRAP_NOERR exception_15, 15
TRAP_NOERR exception_16, 16
TRAP_ERR   exception_17, 17
TRAP_NOERR exception_18, 18
TRAP_NOERR exception_19, 19
TRAP_NOERR exception_20, 20
TRAP_ERR   exception_21, 21
TRAP_NOERR exception_22, 22
TRAP_NOERR exception_23, 23
TRAP_NOERR exception_24, 24
TRAP_NOERR exception_25, 25
TRAP_NOERR exception_26, 26
TRAP_NOERR exception_27, 27
TRAP_NOERR exception_28, 28
TRAP_ERR   exception_29, 29
TRAP_ERR   exception_30, 30
TRAP_NOERR exception_31, 31

TRAP_NOERR irq_interrupt_0,  0x20
TRAP_NOERR irq_interrupt_1,  0x21
TRAP_NOERR irq_interrupt_2,  0x22
TRAP_NOERR irq_interrupt_3,  0x23
TRAP_NOERR irq_interrupt_4,  0x24
TRAP_NOERR irq_interrupt_5,  0x25
TRAP_NOERR irq_interrupt_6,  0x26
TRAP_NOERR irq_interrupt_7,  0x27
TRAP_NOERR irq_interrupt_8,  0x28
TRAP_NOERR irq_interrupt_9,  0x29
TRAP_NOERR irq_interrupt_10, 0x2a
TRAP_NOERR irq_interrupt_11, 0x2b
TRAP_NOERR irq_interrupt_12, 0x2c
TRAP_NOERR irq_interrupt_13, 0x2d
TRAP_NOERR irq_interrupt_14, 0x2e
TRAP_NOERR irq_interrupt_15, 0x2f

TRAP_NOERR irq_interrupt_16, 0x30
TRAP_NOERR irq_interrupt_17, 0x31
TRAP_NOERR irq_interrupt_18, 0x32
TRAP_NOERR irq_interrupt_19, 0x33
TRAP_NOERR irq_interrupt_20, 0x34
TRAP_NOERR irq_interrupt_21, 0x35
TRAP_NOERR irq_interrupt_22, 0x36
TRAP_NOERR irq_interrupt_23, 0x37

TRAP_NOERR irq_interrupt_syscall, 0x80
TRAP_NOERR irq_lapic_timer, 0x40
TRAP_NOERR irq_tlb_shootdown, 0x41
TRAP_NOERR irq_resched, 0x42
TRAP_NOERR irq_lapic_spurious, 0xff

.section .data
.global exception_stub_table
exception_stub_table:
    .quad exception_0,  exception_1,  exception_2,  exception_3
    .quad exception_4,  exception_5,  exception_6,  exception_7
    .quad exception_8,  exception_9,  exception_10, exception_11
    .quad exception_12, exception_13, exception_14, exception_15
    .quad exception_16, exception_17, exception_18, exception_19
    .quad exception_20, exception_21, exception_22, exception_23
    .quad exception_24, exception_25, exception_26, exception_27
    .quad exception_28, exception_29, exception_30, exception_31
.section .text

trap_common:
.ifdef HERMES_UNIKERNEL
    pushq %rax
    pushq %rcx
    movq 32(%rsp), %rax
    cmpq $kcall_entry, %rax
    je 2f
    subq $USER_ADDR_MIN, %rax
    movabsq $(USER_ADDR_LIMIT - USER_ADDR_MIN), %rcx
    cmpq %rcx, %rax
    jb 2f
    movq 56(%rsp), %rax
    andq $-16, %rax
    jmp 3f
2:
    movq %gs:8, %rax
3:
    subq $56, %rax
    movq 16(%rsp), %rcx
    movq %rcx, 0(%rax)
    movq 24(%rsp), %rcx
    movq %rcx, 8(%rax)
    movq 32(%rsp), %rcx
    movq %rcx, 16(%rax)
    movq 40(%rsp), %rcx
    movq %rcx, 24(%rax)
    movq 48(%rsp), %rcx
    movq %rcx, 32(%rax)
    movq 56(%rsp), %rcx
    movq %rcx, 40(%rax)
    movq 64(%rsp), %rcx
    movq %rcx, 48(%rax)
    movq %rsp, %rcx
    movq %rax, %rsp
    movq 8(%rcx), %rax
    movq (%rcx), %rcx
.else
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
.endif
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
    call trap_handler

trap_return:
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

.ifndef HERMES_UNIKERNEL
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
.endif
    addq $16, %rsp
    iretq

.ifdef HERMES_UNIKERNEL
.global syscall_entry
syscall_entry:
    movq %rsp, %gs:16
    movq %gs:8, %rsp
    pushq $0x10
    pushq %gs:16
    pushq %r11
    pushq $0x08
    pushq %rcx
    pushq $0
    pushq $0x80

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

    movq %rsp, %rdi
    call syscall_dispatch
    jmp trap_return

.section .kcall, "ax"
.global kcall_entry
kcall_entry:
    cli
    movq %rsp, %gs:16
    movq %gs:8, %rsp
    movq %gs:16, %rcx
    leaq 8(%rcx), %r11
    movq (%rcx), %rcx
    pushq $0x10
    pushq %r11
    pushq $0x202
    pushq $0x08
    pushq %rcx
    pushq $1
    pushq $0x80

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

    movq %rsp, %rdi
    call syscall_dispatch

    movq 136(%rsp), %rcx
    cmpq 96(%rsp), %rcx
    jne trap_return
    movq 160(%rsp), %r11
    cmpq 32(%rsp), %r11
    jne trap_return
    cmpq $0x202, 152(%rsp)
    jne trap_return
    cmpq $0x08, 144(%rsp)
    jne trap_return

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

    movq 16(%rsp), %rcx
    movq 40(%rsp), %rsp
    sti
    jmp *%rcx

.else

.global syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:16
    movq %gs:8, %rsp
    pushq $0x1b
    pushq %gs:16
    pushq %r11
    pushq $0x23
    pushq %rcx
    pushq $0
    pushq $0x80

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

    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    movq %rsp, %rdi
    call syscall_dispatch

    movq 136(%rsp), %rcx
    cmpq 96(%rsp), %rcx
    jne trap_return
    movq 152(%rsp), %r11
    cmpq 32(%rsp), %r11
    jne trap_return
    testl $0x10100, %r11d
    jnz trap_return
    cmpq $0x23, 144(%rsp)
    jne trap_return
    cmpq $0x1b, 168(%rsp)
    jne trap_return
    movq %rcx, %rax
    shrq $47, %rax
    jnz trap_return

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

    cli
    movq 40(%rsp), %rsp
    swapgs
    sysretq

.endif
