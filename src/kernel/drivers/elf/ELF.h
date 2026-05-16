#ifndef OS_ELF_H
#define OS_ELF_H

enum elf_isa_type {
    ELF_NO_SPECIFIC = 0x00,
    ELF_SPARC = 0x02,
    ELF_386 = 0x03,
    ELF_MIPS = 0x08,
    ELF_POWERPC = 0x14,
    ELF_ARM = 0x28,
    ELF_SUPERH = 0x2A,
    ELF_IA64 = 0x32,
    ELF_X86_64 = 0x3E,
    ELF_AARCH64 = 0xB7,
    ELF_RISCV = 0xF3,
};

#endif //OS_ELF_H