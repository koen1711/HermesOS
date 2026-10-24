#ifndef OS_SYSCALLS_FD_H
#define OS_SYSCALLS_FD_H

struct file_s;

int fd_install(struct file_s* f);
struct file_s* fd_get(int fd);
int fd_close(int fd);

#endif //OS_SYSCALLS_FD_H
