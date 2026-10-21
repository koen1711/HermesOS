.code64

# Every interrupt stub builds a TrapFrame (see interrupts.h) on the stack:
#
#   CPU:            ss, rsp, rflags, cs, rip  (+ error code for some exceptions)
#   vector stub:    error code (0 if the CPU didn't push one), vector number
#   trap_common:    rax, rbx, rcx, rdx, rsi, rdi, rbp, r8 - r15
#
# After that %rsp points at the TrapFrame, which is passed to trap_handler.
# trap_return then restores every register from the frame and does iretq.
#
# The CPU aligns %rsp to 16 bytes before pushing its frame. The full TrapFrame
# is 22 * 8 = 176 bytes, a multiple of 16, so %rsp is still 16-byte aligned at
# the call to trap_handler, as the SysV ABI requires.

.global trap_return

.extern trap_handler

# Stub for a vector where the CPU does not push an error code
.macro TRAP_NOERR name, vector
.global \name
\name:
    pushq $0            # dummy error code, keeps the frame layout the same
    pushq $\vector
    jmp trap_common
.endm

# Stub for a vector where the CPU pushes an error code (8, 10-14, 17, 21, 29, 30)
.macro TRAP_ERR name, vector
.global \name
\name:
    pushq $\vector
    jmp trap_common
.endm

# CPU exceptions 0-31
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

# PIC IRQs 0-15 (IRQ_OFFSET + n)
TRAP_NOERR irq_interrupt_timer,    0x20     # IRQ 0
TRAP_NOERR irq_interrupt_keyboard, 0x21     # IRQ 1
TRAP_NOERR irq_2,                  0x22     # cascade (never raised)
TRAP_NOERR irq_3,                  0x23
TRAP_NOERR irq_4,                  0x24
TRAP_NOERR irq_5,                  0x25
TRAP_NOERR irq_6,                  0x26
TRAP_NOERR irq_7,                  0x27     # also the master's spurious IRQ
TRAP_NOERR irq_8,                  0x28
TRAP_NOERR irq_9,                  0x29
TRAP_NOERR irq_10,                 0x2a
TRAP_NOERR irq_11,                 0x2b
TRAP_NOERR irq_12,                 0x2c
TRAP_NOERR irq_13,                 0x2d
TRAP_NOERR irq_14,                 0x2e     # primary ATA
TRAP_NOERR irq_15,                 0x2f     # secondary ATA, also the slave's spurious IRQ

TRAP_NOERR irq_interrupt_syscall,  0xa0     # SYSCALL_VECTOR

# Stub address for every vector 0-47, idt_initialize puts them in the IDT
.section .data
.global trap_stub_table
trap_stub_table:
    .quad exception_0,  exception_1,  exception_2,  exception_3
    .quad exception_4,  exception_5,  exception_6,  exception_7
    .quad exception_8,  exception_9,  exception_10, exception_11
    .quad exception_12, exception_13, exception_14, exception_15
    .quad exception_16, exception_17, exception_18, exception_19
    .quad exception_20, exception_21, exception_22, exception_23
    .quad exception_24, exception_25, exception_26, exception_27
    .quad exception_28, exception_29, exception_30, exception_31
    .quad irq_interrupt_timer, irq_interrupt_keyboard, irq_2, irq_3
    .quad irq_4,  irq_5,  irq_6,  irq_7
    .quad irq_8,  irq_9,  irq_10, irq_11
    .quad irq_12, irq_13, irq_14, irq_15
.section .text

trap_common:
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

    cld                 # the SysV ABI requires the direction flag clear on calls
    movq %rsp, %rdi     # first argument: TrapFrame*
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

    addq $16, %rsp      # skip vector number and error code
    iretq
