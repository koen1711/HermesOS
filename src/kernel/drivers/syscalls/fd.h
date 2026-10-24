#ifndef OS_SYSCALLS_FD_H
#define OS_SYSCALLS_FD_H

#include <os/stdbool.h>

struct file_s;

int fd_install(struct file_s* f);
int fd_install_from(struct file_s* f, int min_fd, bool cloexec);
int fd_install_at(int fd, struct file_s* f, bool cloexec);
struct file_s* fd_get(int fd);
int fd_close(int fd);
bool fd_cloexec(int fd);
void fd_set_cloexec(int fd, bool on);

#endif //OS_SYSCALLS_FD_H
