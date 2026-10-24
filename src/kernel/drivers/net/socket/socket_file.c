#include <drivers/net/socket/socket_file.h>

#include <drivers/net/socket/socket.h>
#include <drivers/fs/vfs/vfs.h>
#include <drivers/syscalls/fd.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

file_operations_t socket_fops = {
    .read = socket_file_read,
    .write = socket_file_write,
    .close = socket_file_close,
};

static struct socket* socket_of(const struct file_s* f)
{
    return f->node->private_data;
}

ssize_t socket_file_read(struct file_s* f, void* buf, const size_t len)
{
    return sock_recv(socket_of(f), buf, len, 0, nullptr, nullptr);
}

ssize_t socket_file_write(struct file_s* f, const void* buf, const size_t len)
{
    return sock_send(socket_of(f), buf, len, 0, nullptr, 0);
}

int socket_file_close(struct file_s* f)
{
    inode_t* node = f->node;
    struct socket* so = node->private_data;
    so->fd = -1;
    sock_close(so);
    free(node);
    return 0;
}

int socket_file_alloc_fd(struct socket* so)
{
    inode_t* node = malloc(sizeof(inode_t));
    struct file_s* f = malloc(sizeof(struct file_s));
    if (!node || !f) {
        free(node);
        free(f);
        return -ENOMEM;
    }
    memset(node, 0, sizeof(*node));
    node->type = INODE_SOCKET;
    node->fops = &socket_fops;
    node->private_data = so;

    memset(f, 0, sizeof(*f));
    f->node = node;
    f->flags = O_RDONLY | O_WRONLY;
    f->ref_count = 1;

    if (vfs_install(f) < 0) {
        free(node);
        free(f);
        return -ENFILE;
    }
    const int fd = fd_install(f);
    if (fd < 0) {
        node->fops = nullptr;
        vfs_close((int)f->id);
        free(node);
        return fd;
    }
    so->fd = fd;
    return fd;
}

struct socket* socket_from_fd(const int fd)
{
    const struct file_s* f = fd_get(fd);
    if (!f || f->node->fops != &socket_fops)
        return nullptr;
    return socket_of(f);
}
