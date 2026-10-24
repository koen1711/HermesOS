#include "sys.h"

#include "fd.h"
#include <drivers/fs/vfs/vfs.h>
#include <drivers/scheduler/process.h>
#include <hardware/memory/alloc.h>
#include <hardware/memory/vm.h>
#include <os/errno.h>

enum mmap_flags {
    MAP_SHARED = 0x01,
    MAP_PRIVATE = 0x02,
    MAP_TYPE = 0x03,
    MAP_FIXED = 0x10,
    MAP_ANONYMOUS = 0x20,
    MAP_POPULATE = 0x8000,
    MAP_FIXED_NOREPLACE = 0x100000
};

enum madvise_advice {
    MADV_DONTNEED = 4,
    MADV_FREE = 8
};

static struct vm_space* current_vm()
{
    return scheduler_get()->current->vm;
}

static long map_file(struct vm_space* vm, struct file_s* f, const uint64_t start, const size_t len,
                     const int64_t offset)
{
    if ((uint64_t)offset >= f->node->size)
        return 0;
    const size_t avail = f->node->size - (size_t)offset;
    const size_t total = len < avail ? len : avail;

    uint8_t* buf = malloc(PAGE_SIZE);
    if (buf == nullptr)
        return -ENOMEM;
    const size_t saved = f->file_position;
    f->file_position = (size_t)offset;
    long err = 0;
    for (size_t done = 0; err == 0 && done < total;) {
        const size_t want = total - done < PAGE_SIZE ? total - done : PAGE_SIZE;
        const ssize_t n = vfs_read((int)f->id, buf, want);
        if (n <= 0)
            break;
        err = vm_write(vm, start + done, buf, (size_t)n);
        done += (size_t)n;
    }
    f->file_position = saved;
    free(buf);
    return err;
}

long sys_mmap(const uint64_t addr, const size_t len, const int prot, const int flags, const int fd,
              const int64_t offset)
{
    struct vm_space* vm = current_vm();
    const int type = flags & MAP_TYPE;
    if (len == 0 || (offset & (PAGE_SIZE - 1)) || type == 0 || (prot & ~(VM_READ | VM_WRITE | VM_EXEC)))
        return -EINVAL;
    if (PAGE_ALIGN_UP(len) < len)
        return -ENOMEM;

    const bool fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
    if (fixed && (addr & (PAGE_SIZE - 1)))
        return -EINVAL;
    if ((flags & MAP_FIXED_NOREPLACE) && !vm_range_free(vm, addr, addr + PAGE_ALIGN_UP(len)))
        return -EEXIST;

    struct file_s* f = nullptr;
    if (!(flags & MAP_ANONYMOUS)) {
        f = fd_get(fd);
        if (f == nullptr)
            return -EBADF;
        if (f->node->type != INODE_FILE)
            return -ENODEV;
        if (!(f->flags & O_RDONLY))
            return -EACCES;
    }

    const bool shared = type != MAP_PRIVATE;
    uint64_t start;
    long err = vm_map(vm, addr & PAGE_MASK, len, (uint32_t)prot, shared ? VMA_SHARED : 0, fixed, &start);
    if (err)
        return err;
    if (shared)
        err = vm_write(vm, start, nullptr, PAGE_ALIGN_UP(len));
    if (err == 0 && f)
        err = map_file(vm, f, start, len, offset);
    if (err == 0 && (flags & MAP_POPULATE))
        vm_populate(vm, start, len, false);
    if (err) {
        vm_unmap(vm, start, len);
        return err;
    }
    return (long)start;
}

long sys_munmap(const uint64_t addr, const size_t len)
{
    return vm_unmap(current_vm(), addr, len);
}

long sys_mprotect(const uint64_t addr, const size_t len, const int prot)
{
    if (prot & ~(VM_READ | VM_WRITE | VM_EXEC))
        return -EINVAL;
    return vm_protect(current_vm(), addr, len, (uint32_t)prot);
}

long sys_brk(const uint64_t addr)
{
    return (long)vm_brk(current_vm(), addr);
}

long sys_madvise(const uint64_t addr, const size_t len, const int advice)
{
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;
    if (advice == MADV_DONTNEED || advice == MADV_FREE)
        return vm_discard(current_vm(), addr, len);
    return 0;
}
