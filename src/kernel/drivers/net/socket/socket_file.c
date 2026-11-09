#include <drivers/net/socket/socket_file.h>

#include <drivers/net/core/netif.h>
#include <drivers/net/socket/socket.h>
#include <drivers/net/socket/protosw.h>
#include <drivers/syscalls/uaccess.h>
#include <drivers/fs/vfs/vfs.h>
#include <drivers/syscalls/fd.h>
#include <hardware/memory/alloc.h>
#include <hardware/smp/smp.h>
#include <os/errno.h>

static struct poll_head* socket_file_poll_head(struct file_s* f);

file_operations_t socket_fops = {
    .read = socket_file_read,
    .write = socket_file_write,
    .close = socket_file_close,
    .poll = socket_file_poll,
    .poll_head = socket_file_poll_head,
    .ioctl = socket_file_ioctl,
    .poll_mpsafe = true,
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
    struct socket* so = socket_of(f);
    const bool locked = !sock_mpsafe(so) && net_lock_enter();
    netif_tx_batch_begin();
    const ssize_t n = sock_recv(so, buf, len, 0, nullptr, nullptr);
    netif_tx_batch_end();
    if (locked)
        net_lock_exit();
    return n;
}

ssize_t socket_file_write(struct file_s* f, const void* buf, const size_t len)
{
    struct socket* so = socket_of(f);
    struct netbuf* nb = sock_send_prebuild(so, buf, len);
    const bool locked = !sock_mpsafe(so) && net_lock_enter();
    netif_tx_batch_begin();
    const ssize_t n = nb ? sock_send_prebuilt(so, nb, buf, len) : sock_send(so, buf, len, 0, nullptr, 0);
    netif_tx_batch_end();
    if (locked)
        net_lock_exit();
    return n;
}

int socket_file_close(struct file_s* f)
{
    inode_t* node = f->node;
    struct socket* so = node->private_data;
    const bool locked = net_lock_enter();
    sock_lock(so);
    so->fd = -1;
    sock_unlock(so);
    sock_close(so);
    if (locked)
        net_lock_exit();
    free(node);
    return 0;
}

static int socket_poll_locked(const struct file_s* f)
{
    const struct socket* so = socket_of(f);
    int events = so->error ? VFS_POLLERR | VFS_POLLOUT : 0;
    if (so->options & SO_ACCEPTCONN)
        return events | (dlist_empty(&so->complete) ? 0 : VFS_POLLIN);

    if (so->rcv.head || (so->state & SS_CANTRCVMORE))
        events |= VFS_POLLIN;
    const bool stream = so->proto->flags & PR_CONNREQUIRED;
    if ((!stream || (so->state & SS_ISCONNECTED)) && !(so->state & SS_CANTSENDMORE)
        && (stream ? sockbuf_writable(&so->snd) : sockbuf_space(&so->snd) > 0))
        events |= VFS_POLLOUT;
    if (stream && (so->state & SS_CANTRCVMORE) && (so->state & SS_CANTSENDMORE))
        events |= VFS_POLLHUP;
    return events;
}

int socket_file_poll(struct file_s* f)
{
    return socket_poll_locked(f);
}

static struct poll_head* socket_file_poll_head(struct file_s* f)
{
    return &socket_of(f)->ph;
}

void socket_file_set_nonblock(struct file_s* f, const bool on)
{
    struct socket* so = socket_of(f);
    const bool locked = net_lock_enter();
    sock_lock(so);
    if (on) {
        so->state |= SS_NBIO;
        f->status_flags |= O_NONBLOCK_K;
    } else {
        so->state &= ~SS_NBIO;
        f->status_flags &= ~O_NONBLOCK_K;
    }
    sock_unlock(so);
    if (locked)
        net_lock_exit();
}

static int socket_ioctl_locked(struct file_s* f, const unsigned long request, void* arg)
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

int socket_file_ioctl(struct file_s* f, const unsigned long request, void* arg)
{
    return socket_ioctl_locked(f, request, arg);
}

int socket_file_alloc_fd(struct socket* so, const bool nonblock, const bool cloexec)
{
    inode_t* node = malloc(sizeof(inode_t));
    struct file_s* f = malloc(sizeof(struct file_s));
    if (!node || !f) {
        free(node);
        free(f);
        return -ENOMEM;
    }
    memset(node, 0, offsetof(inode_t, path) + 1);
    memset(f, 0, offsetof(struct file_s, filename) + 1);

    node->type = INODE_SOCKET;
    node->fops = &socket_fops;
    node->private_data = so;

    f->node = node;
    f->flags = O_RDONLY | O_WRONLY;
    f->ref_count = 1;
    if (nonblock) {
        so->state |= SS_NBIO;
        f->status_flags |= O_NONBLOCK_K;
    }

    if (vfs_install(f) < 0) {
        free(node);
        free(f);
        return -ENFILE;
    }
    const int fd = cloexec ? fd_install_from(f, 0, true) : fd_install(f);
    if (fd < 0) {
        vfs_uninstall(f);
        free(node);
        free(f);
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
