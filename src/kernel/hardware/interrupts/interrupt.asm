.code64
.global irq_interrupt_timer
.global irq_interrupt_keyboard

.extern handle_pic_interrupt

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