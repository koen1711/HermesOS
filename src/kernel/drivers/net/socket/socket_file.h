#ifndef NET_SOCKET_SOCKET_FILE_H
#define NET_SOCKET_SOCKET_FILE_H

#include <os/types.h>
#include <os/stddef.h>
#include <os/stdbool.h>
#include <drivers/fs/vfs/driver.h>

struct socket;

extern file_operations_t socket_fops;

ssize_t socket_file_read(struct file_s* f, void* buf, size_t len);
ssize_t socket_file_write(struct file_s* f, const void* buf, size_t len);
int socket_file_close(struct file_s* f);
int socket_file_poll(struct file_s* f);
int socket_file_ioctl(struct file_s* f, unsigned long request, void* arg);
void socket_file_set_nonblock(struct file_s* f, bool on);

int socket_file_alloc_fd(struct socket* so);
struct socket* socket_from_fd(int fd);

#endif //NET_SOCKET_SOCKET_FILE_H
