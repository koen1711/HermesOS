#include "fd.h"

#include <drivers/fs/vfs/vfs.h>
#include <scheduler/process.h>
#include <os/errno.h>

int fd_install(struct file_s* f)
{
    process_t* current = scheduler_get()->current;
    if (current == nullptr)
        return (int)f->id;
    for (int i = 0; i < PROC_MAX_FILE_DESCRIPTORS; i++) {
        if (current->file[i] == nullptr) {
            current->file[i] = f;
            return i;
        }
    }
    return -EMFILE;
}

struct file_s* fd_get(const int fd)
{
    const process_t* current = scheduler_get()->current;
    if (current == nullptr)
        return vfs_get_file(fd);
    if (fd < 0 || fd >= PROC_MAX_FILE_DESCRIPTORS)
        return nullptr;
    return current->file[fd];
}

int fd_close(const int fd)
{
    process_t* current = scheduler_get()->current;
    if (current == nullptr)
        return vfs_close(fd) < 0 ? -EBADF : 0;
    struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;
    current->file[fd] = nullptr;
    vfs_close((int)f->id);
    return 0;
}
