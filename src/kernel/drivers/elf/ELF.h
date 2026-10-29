#ifndef OS_ELF_H
#define OS_ELF_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <os/stdbool.h>

struct vm_space;

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

typedef struct {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr;

struct elf_image {
    uint64_t entry;
    uint64_t program_entry;
    uint64_t phdr;
    uint64_t phent;
    uint64_t phnum;
    uint64_t interp_base;
    uint64_t brk_start;
};

bool elf_is_elf(const uint8_t* header, size_t len);
int elf_load(struct vm_space* vm, int fd, const char* root, struct elf_image* out);

#endif //OS_ELF_H
