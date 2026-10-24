.set AP_TRAMPOLINE_BASE, 0x8000
.set CR4_BITS, 0x620                 # PAE | OSFXSR | OSXMMEXCPT, as the BSP sets in start.asm
.set CR0_BITS, 0x80010031            # PG | WP | NE | ET | PE
.set MSR_EFER, 0xC0000080
.set EFER_LME, 0x100

.set OFFSET_GDT_PTR, tramp_gdt_ptr - ap_trampoline_start
.set OFFSET_PM32, ap_pm32 - ap_trampoline_start
.set OFFSET_LM64, ap_lm64 - ap_trampoline_start

.section .text
.global ap_trampoline_start
.global ap_trampoline_end
.global ap_trampoline_cr3
.global ap_trampoline_stack
.global ap_trampoline_entry

.code16
ap_trampoline_start:
    cli
    cld
    movw %cs, %ax
    movw %ax, %ds
    lgdtl OFFSET_GDT_PTR
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $0x08, $(AP_TRAMPOLINE_BASE + OFFSET_PM32)

.code32
ap_pm32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $CR4_BITS, %eax
    movl %eax, %cr4
    movl AP_TRAMPOLINE_BASE + (ap_trampoline_cr3 - ap_trampoline_start), %eax
    movl %eax, %cr3
    movl $MSR_EFER, %ecx
    rdmsr
    orl $EFER_LME, %eax
    wrmsr
    movl $CR0_BITS, %eax
    movl %eax, %cr0
    ljmpl $0x18, $(AP_TRAMPOLINE_BASE + OFFSET_LM64)

.code64
ap_lm64:
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movq AP_TRAMPOLINE_BASE + (ap_trampoline_stack - ap_trampoline_start), %rsp
    movq AP_TRAMPOLINE_BASE + (ap_trampoline_entry - ap_trampoline_start), %rax
    callq *%rax
1:
    cli
    hlt
    jmp 1b

.align 16
tramp_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF         # 0x08: 32-bit code
    .quad 0x00CF92000000FFFF         # 0x10: 32-bit data
    .quad 0x00AF9A000000FFFF         # 0x18: 64-bit code
tramp_gdt_ptr:
    .short tramp_gdt_ptr - tramp_gdt - 1
    .long AP_TRAMPOLINE_BASE + (tramp_gdt - ap_trampoline_start)

.align 8
ap_trampoline_cr3:
    .quad 0
ap_trampoline_stack:
    .quad 0
ap_trampoline_entry:
    .quad 0
ap_trampoline_end:
