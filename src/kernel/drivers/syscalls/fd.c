#include "fd.h"

#include <drivers/fs/vfs/vfs.h>
#include <drivers/scheduler/process.h>
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
    for (int i = min_fd < 0 ? 0 : min_fd; i < PROC_MAX_FILE_DESCRIPTORS; i++) {
        if (files->file[i] == nullptr) {
            files->file[i] = f;
            files->cloexec[i] = cloexec;
            return i;
        }
    }
    return -EMFILE;
}

int fd_install_at(const int fd, struct file_s* f, const bool cloexec)
{
    struct proc_files* files = current_files();
    if (files == nullptr || fd < 0 || fd >= PROC_MAX_FILE_DESCRIPTORS)
        return -EBADF;
    const struct file_s* old = files->file[fd];
    files->file[fd] = f;
    files->cloexec[fd] = cloexec;
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

int fd_close(const int fd)
{
    if (scheduler_get()->current == nullptr)
        return vfs_close(fd) < 0 ? -EBADF : 0;
    struct proc_files* files = current_files();
    struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;
    files->file[fd] = nullptr;
    files->cloexec[fd] = false;
    vfs_close((int)f->id);
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
    if (files && fd >= 0 && fd < PROC_MAX_FILE_DESCRIPTORS && files->file[fd])
        files->cloexec[fd] = on;
}
