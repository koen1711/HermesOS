.set GDT_ENTRY_SIZE, 8
.set GDT_FLAG_FOUR_KILOBYTE_GRANULARITY, 0x8    # (1 << 3)
.set GDT_FLAG_64BIT_MODE, 0x2                   # (1 << 1)
.set GDT_ACCESS_PRESENT, 0x80                   # (1 << 7)
.set GDT_ACCESS_PRIVILEGE_RING0, 0x00           # (0x0 << 5)
.set GDT_ACCESS_DESCRIPTOR_CODE, 0x10           # (1 << 4)
.set GDT_ACCESS_DESCRIPTOR_DATA, 0x10           # (1 << 4)
.set GDT_ACCESS_EXECUTABLE, 0x8                 # (1 << 3)
.set GDT_ACCESS_READABLE_WRITABLE, 0x2          # (1 << 1)

# Pre-calculated GDT entries
.set GDT_FIRST_ENTRY, 0x0000000000000000

# Kernel code entry:
# base=0, limit=0xffffffff,
# flags=GDT_FLAG_FOUR_KILOBYTE_GRANULARITY | GDT_FLAG_64BIT_MODE
# access=GDT_ACCESS_PRESENT | GDT_ACCESS_PRIVILEGE_RING0 | GDT_ACCESS_DESCRIPTOR_CODE | GDT_ACCESS_EXECUTABLE | GDT_ACCESS_READABLE_WRITABLE
.set GDT_ENTRY_CODE, 0x00AF9A000000FFFF

# Kernel data entry:
# base=0, limit=0xffffffff
# flags=GDT_FLAG_FOUR_KILOBYTE_GRANULARITY | GDT_FLAG_64BIT_MODE
# access=GDT_ACCESS_PRESENT | GDT_ACCESS_PRIVILEGE_RING0 | GDT_ACCESS_DESCRIPTOR_DATA | GDT_ACCESS_READABLE_WRITABLE
.set GDT_ENTRY_DATA, 0x00AF92000000FFFF

.set GDT_ALIGNMENT, 0x1000
.set GDT_SIZE, 0x800

# MMU constants
.set PML4_SIZE, 0x1000
.set PML4_ALIGNMENT, 0x1000
.set PML4_ENTRY_SIZE, 8

.set PDPT_SIZE, 0x1000
.set PDPT_ALIGNMENT, 0x1000
.set PDPT_ENTRY_SIZE, 8

.set PAGE_DIRECTORY_SIZE, 0x1000
.set PAGE_DIRECTORY_ALIGNMENT, 0x1000
.set PAGE_DIRECTORY_ENTRY_SIZE, 8

# MMU flags
.set MMU_PRESENT, 0x1        # (1 << 0)
.set MMU_WRITABLE, 0x2       # (1 << 1)
.set MMU_PDE_TWO_MB, 0x80    # (1 << 7)

# Kernel constants
.set PHYSICAL_START, 0x0000000000400000
.set VIRTUAL_START, 0xFFFFFFFF80400000
.set GDT_ENTRY, 1
.set CR0, (0x80000000 | 0x1 | 0x10 | 0x20 | 0x10000) # CR0_PG_ENABLE | CR0_PM_ENABLE | CR0_ET | CR0_NE | CR0_WP
.set CR4, (0x20 | 0x200 | 0x400) # CR4_PAE_ENABLE | CR4_OSFXSR | CR4_OSXMMEXCPT
.set STACK_SIZE, 0x4000
.set STACK_ALIGNMENT, 0x1000
.set HEAP_SIZE, 0x1000000 # (1 << 24)
.set PAGING_SIZE, 0x200000
.set PAGING_ALIGNMENT, 0x200000

# MSR constants
.set MSR_EFER, 0xC0000080
.set MSR_EFER_LME, 0x100     # (1 << 8)
.set MSR_EFER_NXE, 0x800     # (1 << 11) - No-Execute Enable

# Extra definitions
.set TWO_MEGABYTES_SHIFT, 21
.set TWO_MEGABYTES, 0x200000 # (1 << TWO_MEGABYTES_SHIFT)

.code32
.section .bss
.comm pml4, PML4_SIZE, PML4_ALIGNMENT // Page Map Level 4 Table

// High and Low Page Directory Pointer Tables
.comm low_pdpt, PDPT_SIZE, PDPT_ALIGNMENT
.comm high_pdpt, PDPT_SIZE, PDPT_ALIGNMENT

// High and Low Page Directory Tables
.comm low_page_directory_table, PAGE_DIRECTORY_SIZE, PAGE_DIRECTORY_ALIGNMENT
.comm high_page_directory_table, PAGE_DIRECTORY_SIZE, PAGE_DIRECTORY_ALIGNMENT

.comm paging_space, PAGING_SIZE, PAGING_ALIGNMENT // More page table space
.comm tmp_stack, STACK_SIZE, STACK_ALIGNMENT // Temporary Stack

.data
.align GDT_ALIGNMENT // Align GDT properly in memory.
gdt_start:

    .quad GDT_FIRST_ENTRY           // Unused / Invalid Entry
    .quad GDT_ENTRY_CODE     // Kernel Entry
    .quad GDT_ENTRY_DATA

gdt_end:

    // Empty space for more entries.
    .skip (GDT_SIZE - (gdt_end - gdt_start))

gdt_ptr:

    // LGDT instruction requires a pointer to 2 Byte GDT Size + 4 Byte GDT pointer.
    .short GDT_SIZE - 1
    .long gdt_start

.section .text
.global _start // Setup entry point as function.
_start:

    // Set Stack Pointer to point to the end of the stack as the stack grows downwards.
    movl $tmp_stack + STACK_SIZE, %esp

    // Put multiboot structure pointer on the stack
    push $0
    push %ebx

    // PML4 entry for Low PDPT
    movl $low_pdpt, %eax                                                                        // Pointer to Low PDPT
    or $(MMU_PRESENT | MMU_WRITABLE), %eax                                                      // Set Page Present and Writable Flags
    movl %eax, pml4 + (((PHYSICAL_START >> 39) & 0x1FF) * PML4_ENTRY_SIZE)       // Copy Entry

    // PML4 entry for High PDPT
    movl $high_pdpt, %eax                                                                       // Pointer to High PDPT
    or $(MMU_PRESENT | MMU_WRITABLE), %eax                                                      // Set Page Present and Writable Flags
    movl %eax, pml4 + (((VIRTUAL_START >> 39) & 0x1FF) * PML4_ENTRY_SIZE)        // Copy Entry

    // PDPT entry for Low PDT
    movl $low_page_directory_table, %eax                                                        // Pointer to Low PDT
    or $(MMU_PRESENT | MMU_WRITABLE), %eax                                                      // Set Page Present and Writable Flags
    movl %eax, low_pdpt + (((PHYSICAL_START >> 30) & 0x1FF) * PDPT_ENTRY_SIZE)   // Copy Entry

    // PDPT entry for High PDT
    movl $high_page_directory_table, %eax                                                       // Pointer to High PDT
    or $(MMU_PRESENT | MMU_WRITABLE), %eax                                                      // Set Page Present and Writable Flags
    movl %eax, high_pdpt + (((VIRTUAL_START >> 30) & 0x1FF) * PDPT_ENTRY_SIZE)   // Copy Entry

    // Clear ECX Register for later use.
    xor %ecx, %ecx

    // Load physical kernel end address and divide it by 2MB (the page size).
    movl $(_kernel_physical_end + HEAP_SIZE), %esi
    shrl $TWO_MEGABYTES_SHIFT, %esi
    addl $1, %esi

page_directory_table_loop:

    // EAX = 2MB * ECX
    movl $TWO_MEGABYTES, %eax
    mul %ecx

    // Set present, writable, 2MB page size flags.
    or $(MMU_PRESENT | MMU_WRITABLE | MMU_PDE_TWO_MB), %eax

    // Create an entry in High and Low PDT.
    movl %eax, low_page_directory_table(, %ecx, PAGE_DIRECTORY_ENTRY_SIZE)
    movl %eax, high_page_directory_table(, %ecx, PAGE_DIRECTORY_ENTRY_SIZE)

    // Increment counter.
    inc %ecx

    // Loop again if we havent reached the physical kernel end address.
    cmp %esi, %ecx
    jne page_directory_table_loop

    // Load PML4 address in Control Register 3
    movl $pml4, %eax
    movl %eax, %cr3

    // Load Control Register 4 flags.
    movl $CR4, %eax
    movl %eax, %cr4
    // Read EFER MSR
    movl $MSR_EFER, %ecx
    rdmsr

    // Enable Long Mode
    or $MSR_EFER_LME, %eax
    wrmsr

    // Load Control Register 0 flags.
    movl $CR0, %eax
    movl %eax, %cr0

    // Load Global Descriptor Table
    lgdt gdt_ptr

    // Long Jump to 64-bit code
    ljmp $(GDT_ENTRY * GDT_ENTRY_SIZE), $_start64

.code64
.global _start64 // Setup 64-bit entry point as function.
_start64:
    // Set segment selectors
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