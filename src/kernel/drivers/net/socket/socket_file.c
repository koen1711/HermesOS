#include <drivers/net/socket/socket_file.h>

#include <drivers/net/socket/socket.h>
#include <drivers/net/socket/protosw.h>
#include <drivers/syscalls/uaccess.h>
#include <drivers/fs/vfs/vfs.h>
#include <drivers/syscalls/fd.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

file_operations_t socket_fops = {
    .read = socket_file_read,
    .write = socket_file_write,
    .close = socket_file_close,
    .poll = socket_file_poll,
    .ioctl = socket_file_ioctl,
};

#define O_NONBLOCK_K 0x800
#define FIONREAD_K 0x541B
#define FIONBIO_K 0x5421

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

int socket_file_poll(struct file_s* f)
{
    const struct socket* so = socket_of(f);
    int events = so->error ? VFS_POLLERR | VFS_POLLOUT : 0;
    if (so->options & SO_ACCEPTCONN)
        return events | (dlist_empty(&so->complete) ? 0 : VFS_POLLIN);

    if (so->rcv.head || (so->state & SS_CANTRCVMORE))
        events |= VFS_POLLIN;
    const bool stream = so->proto->flags & PR_CONNREQUIRED;
    if ((!stream || (so->state & SS_ISCONNECTED)) && !(so->state & SS_CANTSENDMORE)
        && sockbuf_space(&so->snd) > 0)
        events |= VFS_POLLOUT;
    if (stream && (so->state & SS_CANTRCVMORE) && (so->state & SS_CANTSENDMORE))
        events |= VFS_POLLHUP;
    return events;
}

void socket_file_set_nonblock(struct file_s* f, const bool on)
{
    struct socket* so = socket_of(f);
    if (on) {
        so->state |= SS_NBIO;
        f->status_flags |= O_NONBLOCK_K;
    } else {
        so->state &= ~SS_NBIO;
        f->status_flags &= ~O_NONBLOCK_K;
    }
}

int socket_file_ioctl(struct file_s* f, const unsigned long request, void* arg)
{
    switch (request) {
    case FIONREAD_K: {
        const int n = (int)socket_of(f)->rcv.cc;
        return copy_to_user(arg, &n, sizeof(n));
    }
    case FIONBIO_K: {
        int on;
        if (copy_from_user(&on, arg, sizeof(on)) < 0)
            return -EFAULT;
        socket_file_set_nonblock(f, on != 0);
        return 0;
    }
    default:
        return -ENOTTY;
    }
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
