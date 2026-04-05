# LIST OF CONSTANTS
.set MAGIC,    0xE85250D6 # 'Magic'

.section .multiboot
header_start:
    .LONG MAGIC
    .LONG 0 # 32-bit, 0
    .LONG header_end - header_start # header_length
    .LONG -(MAGIC + 0 + (header_end - header_start)) # Checksum

    # Request Memory Map
    .SHORT 4 # type = 4 (memory map request)
    .SHORT 0 # flags = 0 (optional = false)
    .LONG 8  # size = 8 bytes

    # Mandatory end tag
    .SHORT 0 # 16-bit, type `0`
    .SHORT 0 # 16-bit, flag
    .LONG 8 # 32-bit, 8
header_end:
