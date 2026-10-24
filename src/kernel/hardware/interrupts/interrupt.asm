.code64

.global trap_return

.extern trap_handler

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

TRAP_NOERR exception_0,  0      # divide error
TRAP_NOERR exception_1,  1      # debug
TRAP_NOERR exception_2,  2      # NMI
TRAP_NOERR exception_3,  3      # breakpoint
TRAP_NOERR exception_4,  4      # overflow
TRAP_NOERR exception_5,  5      # bound range
TRAP_NOERR exception_6,  6      # invalid opcode
TRAP_NOERR exception_7,  7      # device not available
TRAP_ERR   exception_8,  8      # double fault
TRAP_NOERR exception_9,  9      # coprocessor segment overrun
TRAP_ERR   exception_10, 10     # invalid TSS
TRAP_ERR   exception_11, 11     # segment not present
TRAP_ERR   exception_12, 12     # stack fault
TRAP_ERR   exception_13, 13     # general protection
TRAP_ERR   exception_14, 14     # page fault
TRAP_NOERR exception_15, 15     # reserved
TRAP_NOERR exception_16, 16     # x87 floating point
TRAP_ERR   exception_17, 17     # alignment check
TRAP_NOERR exception_18, 18     # machine check
TRAP_NOERR exception_19, 19     # SIMD floating point
TRAP_NOERR exception_20, 20     # virtualization
TRAP_ERR   exception_21, 21     # control protection
TRAP_NOERR exception_22, 22     # reserved
TRAP_NOERR exception_23, 23
TRAP_NOERR exception_24, 24
TRAP_NOERR exception_25, 25
TRAP_NOERR exception_26, 26
TRAP_NOERR exception_27, 27
TRAP_NOERR exception_28, 28
TRAP_ERR   exception_29, 29     # VMM communication
TRAP_ERR   exception_30, 30     # security
TRAP_NOERR exception_31, 31     # reserved

# PIC IRQs 0-15 (IRQ_OFFSET + n), irq_register_handler puts them in the IDT
TRAP_NOERR irq_interrupt_0,  0x20      # PIT
TRAP_NOERR irq_interrupt_1,  0x21      # PS/2 keyboard
TRAP_NOERR irq_interrupt_2,  0x22      # cascade (never raised)
TRAP_NOERR irq_interrupt_3,  0x23
TRAP_NOERR irq_interrupt_4,  0x24
TRAP_NOERR irq_interrupt_5,  0x25
TRAP_NOERR irq_interrupt_6,  0x26
TRAP_NOERR irq_interrupt_7,  0x27      # also the master's spurious IRQ
TRAP_NOERR irq_interrupt_8,  0x28
TRAP_NOERR irq_interrupt_9,  0x29
TRAP_NOERR irq_interrupt_10, 0x2a
TRAP_NOERR irq_interrupt_11, 0x2b
TRAP_NOERR irq_interrupt_12, 0x2c
TRAP_NOERR irq_interrupt_13, 0x2d
TRAP_NOERR irq_interrupt_14, 0x2e      # primary ATA
TRAP_NOERR irq_interrupt_15, 0x2f      # secondary ATA, also the slave's spurious IRQ

TRAP_NOERR irq_interrupt_syscall, 0x80 # SYSCALL_VECTOR, syscalls_initialize puts it in the IDT
TRAP_NOERR irq_lapic_timer, 0x40       # LAPIC_TIMER_VECTOR, smp_initialize puts these in the IDT
TRAP_NOERR irq_tlb_shootdown, 0x41
TRAP_NOERR irq_resched, 0x42
TRAP_NOERR irq_lapic_spurious, 0xff

# Stub address for every exception 0-31, idt_initialize puts them in the IDT
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
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
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

    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
    addq $16, %rsp
    iretq

.global syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:16   # cpu->user_rsp
    movq %gs:8, %rsp    # cpu->kernel_rsp
    pushq $0x1b         # ss: user data | 3
    pushq %gs:16        # rsp
    pushq %r11          # rflags
    pushq $0x23         # cs: user code | 3
    pushq %rcx          # rip
    pushq $0            # error code
    pushq $0x80         # SYSCALL_VECTOR

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

    movq 136(%rsp), %rcx    # rip
    cmpq 96(%rsp), %rcx     # rcx
    jne trap_return
    movq 152(%rsp), %r11    # rflags
    cmpq 32(%rsp), %r11     # r11
    jne trap_return
    cmpq $0x23, 144(%rsp)   # cs
    jne trap_return
    cmpq $0x1b, 168(%rsp)   # ss
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
