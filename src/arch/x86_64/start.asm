.set GDT_ENTRY_SIZE, 8
.set GDT_FLAG_FOUR_KILOBYTE_GRANULARITY, 0x8
.set GDT_FLAG_64BIT_MODE, 0x2
.set GDT_ACCESS_PRESENT, 0x80
.set GDT_ACCESS_PRIVILEGE_RING0, 0x00
.set GDT_ACCESS_DESCRIPTOR_CODE, 0x10
.set GDT_ACCESS_DESCRIPTOR_DATA, 0x10
.set GDT_ACCESS_EXECUTABLE, 0x8
.set GDT_ACCESS_READABLE_WRITABLE, 0x2

.set GDT_FIRST_ENTRY, 0x0000000000000000

.set GDT_ENTRY_CODE, 0x00AF9A000000FFFF

.set GDT_ENTRY_DATA, 0x00AF92000000FFFF

.set GDT_ENTRY_USER_DATA, 0x00AFF2000000FFFF

.set GDT_ENTRY_USER_CODE, 0x00AFFA000000FFFF

.set GDT_ALIGNMENT, 0x1000
.set GDT_SIZE, 0x800

.set PML4_SIZE, 0x1000
.set PML4_ALIGNMENT, 0x1000
.set PML4_ENTRY_SIZE, 8

.set PDPT_SIZE, 0x1000
.set PDPT_ALIGNMENT, 0x1000
.set PDPT_ENTRY_SIZE, 8

.set PAGE_DIRECTORY_SIZE, 0x1000
.set PAGE_DIRECTORY_ALIGNMENT, 0x1000
.set PAGE_DIRECTORY_ENTRY_SIZE, 8
.set PAGE_DIRECTORY_ENTRIES, 512

.set MMU_PRESENT, 0x1
.set MMU_WRITABLE, 0x2
.set MMU_PDE_TWO_MB, 0x80

.set PHYSICAL_START, 0x0000000000400000
.set VIRTUAL_START, 0xFFFFFFFF80400000
.set GDT_ENTRY, 1
.set CR0, (0x80000000 | 0x1 | 0x10 | 0x20 | 0x10000)
.set CR4, (0x20 | 0x200 | 0x400)
.set STACK_SIZE, 0x4000
.set STACK_ALIGNMENT, 0x1000
.set HEAP_SIZE, 0x40000000
.set PAGING_SIZE, 0x200000
.set PAGING_ALIGNMENT, 0x200000

.set MSR_EFER, 0xC0000080
.set MSR_EFER_LME, 0x100
.set MSR_EFER_NXE, 0x800

.set TWO_MEGABYTES_SHIFT, 21
.set TWO_MEGABYTES, 0x200000

.code32
.section .bss
.comm pml4, PML4_SIZE, PML4_ALIGNMENT

.comm low_pdpt, PDPT_SIZE, PDPT_ALIGNMENT
.comm high_pdpt, PDPT_SIZE, PDPT_ALIGNMENT

.comm low_page_directory_table, PAGE_DIRECTORY_SIZE, PAGE_DIRECTORY_ALIGNMENT
.comm high_page_directory_table, PAGE_DIRECTORY_SIZE, PAGE_DIRECTORY_ALIGNMENT

.comm paging_space, PAGING_SIZE, PAGING_ALIGNMENT
.comm tmp_stack, STACK_SIZE, STACK_ALIGNMENT

.data
.global gdt_start
gdt_start:
    .quad GDT_FIRST_ENTRY
    .quad GDT_ENTRY_CODE
    .quad GDT_ENTRY_DATA
    .quad GDT_ENTRY_USER_DATA
    .quad GDT_ENTRY_USER_CODE
    .quad 0
    .quad 0
gdt_end:
    .skip (GDT_SIZE - (gdt_end - gdt_start))
gdt_ptr:
    .short GDT_SIZE - 1
    .long gdt_start

.section .text
.global _start
_start:

check_cpuid:
    pushfl
    pushfl
    xorl $0x00200000, (%esp)
    popfl
    pushfl
    popl %eax
    xorl (%esp), %eax
    popfl
    andl $0x00200000, %eax
    jz cpuid_not_supported

    push %ebx

    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb no_long_mode

    mov $0x80000001, %eax
    cpuid
    test $0x20000000, %edx
    jz no_long_mode

    pop %ebx

    movl $tmp_stack + STACK_SIZE, %esp

    push $0
    push %ebx

    movl $low_pdpt, %eax
    or $(MMU_PRESENT | MMU_WRITABLE), %eax
    movl %eax, pml4 + (((PHYSICAL_START >> 39) & 0x1FF) * PML4_ENTRY_SIZE)

    movl $high_pdpt, %eax
    or $(MMU_PRESENT | MMU_WRITABLE), %eax
    movl %eax, pml4 + (((VIRTUAL_START >> 39) & 0x1FF) * PML4_ENTRY_SIZE)

    movl $low_page_directory_table, %eax
    or $(MMU_PRESENT | MMU_WRITABLE), %eax
    movl %eax, low_pdpt + (((PHYSICAL_START >> 30) & 0x1FF) * PDPT_ENTRY_SIZE)

    movl $high_page_directory_table, %eax
    or $(MMU_PRESENT | MMU_WRITABLE), %eax
    movl %eax, high_pdpt + (((VIRTUAL_START >> 30) & 0x1FF) * PDPT_ENTRY_SIZE)

    xor %ecx, %ecx

    movl $(_kernel_physical_end + HEAP_SIZE), %esi
    shrl $TWO_MEGABYTES_SHIFT, %esi
    addl $1, %esi

    cmpl $PAGE_DIRECTORY_ENTRIES, %esi
    jbe 1f
    movl $PAGE_DIRECTORY_ENTRIES, %esi
1:

page_directory_table_loop:
    movl $TWO_MEGABYTES, %eax
    mul %ecx

    or $(MMU_PRESENT | MMU_WRITABLE | MMU_PDE_TWO_MB), %eax

    movl %eax, low_page_directory_table(, %ecx, PAGE_DIRECTORY_ENTRY_SIZE)
    movl %eax, high_page_directory_table(, %ecx, PAGE_DIRECTORY_ENTRY_SIZE)

    inc %ecx

    cmp %esi, %ecx
    jne page_directory_table_loop

    movl $pml4, %eax
    movl %eax, %cr3

    movl $CR4, %eax
    movl %eax, %cr4

    movl $MSR_EFER, %ecx
    rdmsr

    or $MSR_EFER_LME, %eax
    wrmsr

    movl $CR0, %eax
    movl %eax, %cr0

    lgdt gdt_ptr
    ljmp $(GDT_ENTRY * GDT_ENTRY_SIZE), $_start64

no_support:
    mov $0xB8000, %edi
    movw $0x0C4E,   (%edi)
    movw $0x0C4F,  2(%edi)
    movw $0x0C53,  4(%edi)
    movw $0x0C55,  6(%edi)
    movw $0x0C50,  8(%edi)
    movw $0x0C50, 10(%edi)
    movw $0x0C4F, 12(%edi)
    movw $0x0C52, 14(%edi)
    movw $0x0C54, 16(%edi)

    ret
cpuid_not_supported:
    call no_support

    cli
    hlt
    jmp cpuid_not_supported

no_long_mode:
    call no_support

    cli
    hlt
    jmp no_long_mode

.code64
.global _start64
_start64:
    mov $0, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    pop %rdi

    call kernel_main
end_loop:
    cli
    hlt
    jmp end_loop
