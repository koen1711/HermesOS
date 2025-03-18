ENTRY(_start)

SECTIONS
{
    . = 4M;
    _kernel_physical_start = .;

    .boottext :
    {
        *(.multiboot)
        *(.text)
    }

    .bootrodata :
    {
        *(.rodata)
    }

    .bootdata :
    {
        *(.data)
    }

    .bootbss :
    {
        *(.bss)
        *(COMMON)
    }

    . = ALIGN(0x1000);
    _boot_end = .;

    _kernel_virtual_start = .;

    .text : AT(_boot_end)
    {
        *(.text)
    }

    . = ALIGN(0x1000);

    .rodata : AT ( (LOADADDR (.text) + SIZEOF (.text) + 0xFFF) & 0xFFFFFFFFFFFFF000 )
    {
        *(.rodata)
        *(.rodata.*)
    }

    . = ALIGN(0x1000);

    .data : AT ( (LOADADDR (.rodata) + SIZEOF (.rodata) + 0xFFF) & 0xFFFFFFFFFFFFF000 )
    {
        *(.data)
        *(.eh_frame_hdr)
        *(.eh_frame)
    }

    . = ALIGN(0x1000);

    .bss : AT ( (LOADADDR (.data) + SIZEOF (.data) + 0xFFF) & 0xFFFFFFFFFFFFF000 )
    {
        *(COMMON)
        *(.bss)
    }

    /DISCARD/ :
    {
        *(.comment)
    }

    _kernel_virtual_end = .;

    _kernel_physical_end = (LOADADDR (.bss) + SIZEOF (.bss) + 0xFFF) & 0xFFFFFFFFFFFFF000;
}