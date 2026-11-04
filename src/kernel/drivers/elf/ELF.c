#include "ELF.h"

#include <drivers/fs/vfs/vfs.h>
#include <hardware/memory/alloc.h>
#include <hardware/memory/vm.h>
#include <os/errno.h>

#define ET_EXEC 2
#define ET_DYN 3
#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define ELF_MAX_PHDRS 64

bool elf_is_elf(const uint8_t* header, const size_t len)
{
    return len >= 4 && header[0] == 0x7F && header[1] == 'E' && header[2] == 'L' && header[3] == 'F';
}

static int read_at(const int fd, const uint64_t offset, void* buf, const size_t len)
{
    if (vfs_seek(fd, (int64_t)offset, SEEK_SET) < 0)
        return -EIO;
    size_t done = 0;
    while (done < len) {
        const ssize_t n = vfs_read(fd, (uint8_t*)buf + done, len - done);
        if (n <= 0)
            return -ENOEXEC;
        done += (size_t)n;
    }
    return 0;
}

static uint32_t segment_prot(const uint32_t flags)
{
    return (flags & PF_R ? VM_READ : 0) | (flags & PF_W ? VM_WRITE : 0) | (flags & PF_X ? VM_EXEC : 0);
}

static int load_segment(struct vm_space* vm, const int fd, const elf64_phdr* ph, const uint64_t base)
{
    const uint64_t va = base + ph->p_vaddr;
    const uint64_t start = va & PAGE_MASK;
    const uint64_t end = PAGE_ALIGN_UP(va + ph->p_memsz);
    if (ph->p_filesz > ph->p_memsz || end < start || start < USER_ADDR_MIN || end > USER_ADDR_LIMIT)
        return -ENOEXEC;

    const uint32_t prot = segment_prot(ph->p_flags);
    uint64_t map_start = start;

    const struct vm_area* prev = vm_find(vm, start);
    if (prev) {
        const int err = vm_protect(vm, start, PAGE_SIZE, prev->prot | prot);
        if (err)
            return err;
        map_start += PAGE_SIZE;
    }
    if (map_start < end) {
        const int err = vm_map(vm, map_start, end - map_start, prot, 0, true, nullptr);
        if (err)
            return err;
    }

    uint8_t* buf = malloc(PAGE_SIZE);
    if (!buf)
        return -ENOMEM;
    int err = 0;
    for (uint64_t done = 0; err == 0 && done < ph->p_filesz;) {
        const size_t n = ph->p_filesz - done < PAGE_SIZE ? ph->p_filesz - done : PAGE_SIZE;
        err = read_at(fd, ph->p_offset + done, buf, n);
        if (err == 0)
            err = vm_write(vm, va + done, buf, n);
        done += n;
    }
    free(buf);
    return err;
}

static int load_object(struct vm_space* vm, const int fd, const uint64_t dyn_base, const bool allow_interp,
                       struct elf_image* out, char* interp, const size_t interp_len)
{
    elf64_ehdr eh;
    int err = read_at(fd, 0, &eh, sizeof(eh));
    if (err)
        return err;
    if (!elf_is_elf(eh.e_ident, sizeof(eh.e_ident)) || eh.e_ident[4] != 2 || eh.e_ident[5] != 1
        || eh.e_machine != ELF_X86_64 || (eh.e_type != ET_EXEC && eh.e_type != ET_DYN)
        || eh.e_phentsize != sizeof(elf64_phdr) || eh.e_phnum == 0 || eh.e_phnum > ELF_MAX_PHDRS)
        return -ENOEXEC;

    elf64_phdr* ph = malloc(eh.e_phnum * sizeof(elf64_phdr));
    if (!ph)
        return -ENOMEM;
    err = read_at(fd, eh.e_phoff, ph, eh.e_phnum * sizeof(elf64_phdr));

    const uint64_t base = eh.e_type == ET_DYN ? dyn_base : 0;
    uint64_t phdr = 0;
    uint64_t top = 0;
    for (int i = 0; err == 0 && i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {
            if (!allow_interp || ph[i].p_filesz == 0 || ph[i].p_filesz >= interp_len)
                err = -ENOEXEC;
            else if ((err = read_at(fd, ph[i].p_offset, interp, ph[i].p_filesz)) == 0)
                interp[ph[i].p_filesz] = '\0';
        } else if (ph[i].p_type == PT_PHDR) {
            phdr = base + ph[i].p_vaddr;
        } else if (ph[i].p_type == PT_LOAD) {
            err = load_segment(vm, fd, &ph[i], base);
            if (!phdr && eh.e_phoff >= ph[i].p_offset && eh.e_phoff < ph[i].p_offset + ph[i].p_filesz)
                phdr = base + ph[i].p_vaddr + (eh.e_phoff - ph[i].p_offset);
            if (base + ph[i].p_vaddr + ph[i].p_memsz > top)
                top = base + ph[i].p_vaddr + ph[i].p_memsz;
        }
    }
    free(ph);
    if (err)
        return err;

    out->entry = base + eh.e_entry;
    out->program_entry = out->entry;
    out->phdr = phdr;
    out->phent = sizeof(elf64_phdr);
    out->phnum = eh.e_phnum;
    out->brk_start = PAGE_ALIGN_UP(top);
    return 0;
}

int elf_load(struct vm_space* vm, const int fd, const char* root, struct elf_image* out)
{
    char* interp = malloc(2 * MAX_PATH_LENGTH);
    if (!interp)
        return -ENOMEM;
    interp[0] = '\0';
    char* interp_path = interp + MAX_PATH_LENGTH;

    int err = load_object(vm, fd, USER_PIE_BASE, true, out, interp, MAX_PATH_LENGTH);
    if (err == 0 && interp[0] != '\0')
        err = vfs_resolve_root(root, "/", interp, interp_path);
    if (err == 0 && interp[0] != '\0') {
        const int ifd = vfs_open(interp_path, O_RDONLY);
        struct elf_image ld = {0};
        err = ifd < 0 ? -ENOENT : load_object(vm, ifd, USER_INTERP_BASE, false, &ld, nullptr, 0);
        if (ifd >= 0)
            vfs_close(ifd);
        if (err == 0) {
            out->entry = ld.entry;
            out->interp_base = USER_INTERP_BASE;
        }
    }
    free(interp);
    return err;
}
