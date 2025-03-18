# LIST OF CONSTANTS
.set ALIGN,    1<<0             /* align loaded modules on page boundaries */
.set MEMINFO,  1<<1             /* provide memory map */
.set FLAGS,    ALIGN | MEMINFO  /* this is the Multiboot 'flag' field */
.set MAGIC,    0xe85250d6       /* 'magic number' lets bootloader find the header */
.set CHECKSUM, -(MAGIC + FLAGS) /* checksum of above, to prove we are multiboot */

.section .multiboot # this is the header of the multiboot
header_start:
    .LONG MAGIC
    .LONG 0 // 32-bit, 0
    .LONG header_end - header_start // header_length
    .LONG - (MAGIC + 0 + (header_end - header_start)) // checksum

    // Mandatory end tag
    .SHORT 0 // 16-bit, type `0`
	.SHORT 0 // 16-bit, flag
	.LONG 8 // 32-bit, 8
header_end: