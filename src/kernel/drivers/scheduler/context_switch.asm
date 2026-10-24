.code64
.section .text

.global context_switch
context_switch:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, (%rdi)
    movq %rsi, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx

    ret

.global process_entry
process_entry:
    call process_entry_unlock
    jmp trap_return

.global kthread_entry
kthread_entry:
    call process_entry_unlock
    movq %r13, %rdi
    call *%r12
    ud2
