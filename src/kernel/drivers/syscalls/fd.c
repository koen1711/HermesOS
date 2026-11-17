#include "fd.h"

#include <drivers/fs/vfs/vfs.h>
#include <drivers/scheduler/process.h>
#include <hardware/interrupts/irq.h>
#include <hardware/smp/smp.h>
#include <os/errno.h>

static struct proc_files* current_files()
{
    const process_t* current = scheduler_get()->current;
    return current ? current->files : nullptr;
}

int fd_install(struct file_s* f)
{
    if (scheduler_get()->current == nullptr)
        return (int)f->id;
    return fd_install_from(f, 0, false);
}

int fd_install_from(struct file_s* f, const int min_fd, const bool cloexec)
{
    struct proc_files* files = current_files();
    if (files == nullptr)
        return -EBADF;
    uint64_t flags;
    spinlock_acquire_irqsave(&files->lock, &flags);
    for (int i = min_fd < 0 ? 0 : min_fd; i < PROC_MAX_FILE_DESCRIPTORS; i++) {
        if (files->file[i] == nullptr) {
            files->file[i] = f;
            files->cloexec[i] = cloexec;
            spinlock_release_irqrestore(&files->lock, flags);
            return i;
        }
    }
    spinlock_release_irqrestore(&files->lock, flags);
    return -EMFILE;
}

int fd_install_at(const int fd, struct file_s* f, const bool cloexec)
{
    struct proc_files* files = current_files();
    if (files == nullptr || fd < 0 || fd >= PROC_MAX_FILE_DESCRIPTORS)
        return -EBADF;
    uint64_t flags;
    spinlock_acquire_irqsave(&files->lock, &flags);
    const struct file_s* old = files->file[fd];
    files->file[fd] = f;
    files->cloexec[fd] = cloexec;
    spinlock_release_irqrestore(&files->lock, flags);
    if (old != nullptr)
        vfs_close((int)old->id);
    return fd;
}

struct file_s* fd_get(const int fd)
{
    if (scheduler_get()->current == nullptr)
        return vfs_get_file(fd);
    const struct proc_files* files = current_files();
    if (files == nullptr || fd < 0 || fd >= PROC_MAX_FILE_DESCRIPTORS)
        return nullptr;
    return files->file[fd];
}

static bool file_tryget(struct file_s* f)
{
    int n = __atomic_load_n(&f->ref_count, __ATOMIC_ACQUIRE);
    while (n > 0)
        if (__atomic_compare_exchange_n(&f->ref_count, &n, n + 1, true, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
            return true;
    return false;
}

struct file_s* fd_pin(const int fd)
{
    struct proc_files* files = current_files();
    if (files == nullptr || fd < 0 || fd >= PROC_MAX_FILE_DESCRIPTORS)
        return nullptr;
    for (;;) {
        const uint64_t flags = irq_save();
        struct cpu* cpu = cpu_this();
        __atomic_store_n(&cpu->fd_lookup, true, __ATOMIC_SEQ_CST);
        struct file_s* f = __atomic_load_n(&files->file[fd], __ATOMIC_ACQUIRE);
        const bool got = f && file_tryget(f);
        __atomic_store_n(&cpu->fd_lookup, false, __ATOMIC_RELEASE);
        irq_restore(flags);
        if (f == nullptr)
            return nullptr;
        if (got) {
            if (__atomic_load_n(&files->file[fd], __ATOMIC_ACQUIRE) == f)
                return f;
            vfs_file_put(f);
        }
    }
}

void fd_pin_quiesce()
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (int i = 0; i < cpu_count(); i++) {
        const struct cpu* cpu = cpu_get(i);
        while (__atomic_load_n(&cpu->fd_lookup, __ATOMIC_ACQUIRE)) {
            smp_spin_poll();
            asm volatile("pause");
        }
    }
}

int fd_close(const int fd)
{
    if (scheduler_get()->current == nullptr)
        return vfs_close(fd) < 0 ? -EBADF : 0;
    struct proc_files* files = current_files();
    if (files == nullptr || fd < 0 || fd >= PROC_MAX_FILE_DESCRIPTORS)
        return -EBADF;
    uint64_t flags;
    spinlock_acquire_irqsave(&files->lock, &flags);
    struct file_s* f = files->file[fd];
    if (f == nullptr) {
        spinlock_release_irqrestore(&files->lock, flags);
        return -EBADF;
    }
    files->file[fd] = nullptr;
    files->cloexec[fd] = false;
    spinlock_release_irqrestore(&files->lock, flags);
    vfs_file_put(f);
    return 0;
}

bool fd_cloexec(const int fd)
{
    const struct proc_files* files = current_files();
    return files && fd >= 0 && fd < PROC_MAX_FILE_DESCRIPTORS && files->cloexec[fd];
}

void fd_set_cloexec(const int fd, const bool on)
{
    struct proc_files* files = current_files();
    if (!files || fd < 0 || fd >= PROC_MAX_FILE_DESCRIPTORS)
        return;
    uint64_t flags;
    spinlock_acquire_irqsave(&files->lock, &flags);
    if (files->file[fd])
        files->cloexec[fd] = on;
    spinlock_release_irqrestore(&files->lock, flags);
}
