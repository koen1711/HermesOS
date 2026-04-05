.code64
.equ ENOSYS, 38

.global irq_interrupt_timer
.global irq_interrupt_keyboard

.extern handle_pic_interrupt

syscall_interrupt:
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

    # In 64-bit mode, we don't typically save segment registers
    # as they're mostly ignored, but if you need them:
    # Note: You'll need to use movw to save them to memory or stack

    # Push error code or syscall number
    pushq $-ENOSYS  # Use immediate value with pushq

    # Call the syscall handler
    # In 64-bit calling convention, arguments are passed in registers
    # You might need to set up arguments in %rdi, %rsi, etc.
    #call handle_syscall

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

irq_interrupt_timer:
    pushq $0 # Push the interrupt number
    jmp irq_interrupt

irq_interrupt_keyboard:
    pushq $1 # Push the interrupt number
    jmp irq_interrupt

irq_interrupt:
    # First create space for our InterruptContext structure
    sub $152, %rsp  # 19 registers * 8 bytes = 152 bytes (including interrupt_number)

    # Save interrupt number (which we pushed at entry)
    mov 152(%rsp), %rax    # Get the interrupt number we pushed
    mov %rax, 144(%rsp)    # Save to context.interrupt_number

    # Save rflags (pushed by CPU during interrupt)
    mov 160(%rsp), %rax    # rflags is at [original_rsp + 8 + 152]
    mov %rax, 136(%rsp)    # Save to context.rflags

    # Save cs (pushed by CPU during interrupt)
    mov 168(%rsp), %rax    # cs is at [original_rsp + 16 + 152]
    mov %rax, 128(%rsp)    # Save to context.cs

    # Save rip (pushed by CPU during interrupt)
    mov 176(%rsp), %rax    # rip is at [original_rsp + 24 + 152]
    mov %rax, 120(%rsp)    # Save to context.rip

    # Save general-purpose registers
    mov %r15, 112(%rsp)    # context.r15
    mov %r14, 104(%rsp)    # context.r14
    mov %r13, 96(%rsp)     # context.r13
    mov %r12, 88(%rsp)     # context.r12
    mov %r11, 80(%rsp)     # context.r11
    mov %r10, 72(%rsp)     # context.r10
    mov %r9, 64(%rsp)      # context.r9
    mov %r8, 56(%rsp)      # context.r8
    mov %rdi, 48(%rsp)     # context.rdi
    mov %rsi, 40(%rsp)     # context.rsi

    # rsp points to original stack before interrupt
    lea 160(%rsp), %rax    # 152 + 8 for the interrupt number we pushed
    mov %rax, 32(%rsp)     # context.rsp

    mov %rbp, 24(%rsp)     # context.rbp
    mov %rdx, 16(%rsp)     # context.rdx
    mov %rcx, 8(%rsp)      # context.rcx
    mov %rbx, (%rsp)       # context.rbx

    # Save rax separately since we've been using it
    mov %rax, %rbx         # Temporarily store rax value
    sub $8, %rsp
    mov %rbx, (%rsp)       # context.rax

    # Now pass the pointer to the structure as the first argument
    # In x86-64 calling convention, first argument goes in rdi
    lea 8(%rsp), %rdi      # Point to start of our structure

    # Call the C function
    call handle_pic_interrupt

    # Restore rax
    mov (%rsp), %rax
    add $8, %rsp

    # Restore the rest of the registers (context is now gone)
    add $152, %rsp         # Skip the InterruptContext structure
    add $8, %rsp           # Skip the interrupt number we pushed
    iretq