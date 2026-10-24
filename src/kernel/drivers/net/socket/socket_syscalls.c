#include <drivers/net/socket/socket_syscalls.h>

#include <drivers/net/socket/socket.h>
#include <drivers/net/socket/socket_file.h>
#include <drivers/net/socket/protosw.h>
#include <drivers/net/unix/unix.h>
#include <drivers/syscalls/fd.h>
#include <drivers/syscalls/uaccess.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

#define SOCKOPT_MAX 256
#define SOCKET_SSIZE_MAX ((size_t)~0 >> 1)

static long sock_lookup(const int fd, struct socket** out)
{
    *out = nullptr;
    if (!fd_get(fd))
        return -EBADF;
    *out = socket_from_fd(fd);
    return *out ? 0 : -ENOTSOCK;
}

static long addr_in(struct sockaddr_storage* ss, const struct sockaddr* uaddr, const socklen_t len)
{
    if (len > sizeof(*ss))
        return -EINVAL;
    memset(ss, 0, sizeof(*ss));
    return copy_from_user(ss, uaddr, len);
}

static long addr_out(const struct sockaddr_storage* ss, const socklen_t klen, struct sockaddr* uaddr,
                     socklen_t* ulen)
{
    socklen_t room;
    if (copy_from_user(&room, ulen, sizeof(room)) < 0)
        return -EFAULT;
    const socklen_t n = klen < room ? klen : room;
    if (copy_to_user(uaddr, ss, n) < 0 || copy_to_user(ulen, &klen, sizeof(klen)) < 0)
        return -EFAULT;
    return 0;
}

static long addr_room(const struct sockaddr* uaddr, const socklen_t* ulen)
{
    if (!uaddr && !ulen)
        return 0;
    if (!uaddr || !ulen)
        return -EFAULT;
    socklen_t room;
    if (copy_from_user(&room, ulen, sizeof(room)) < 0)
        return -EFAULT;
    if ((int32_t)room < 0)
        return -EINVAL;
    return uaccess_ok_write(uaddr, room) ? 0 : -EFAULT;
}

static int sock_install(struct socket* so, const int tflags)
{
    const int fd = socket_file_alloc_fd(so);
    if (fd < 0) {
        sock_close(so);
        return fd;
    }
    if (tflags & SOCK_NONBLOCK)
        socket_file_set_nonblock(fd_get(fd), true);
    fd_set_cloexec(fd, tflags & SOCK_CLOEXEC);
    return fd;
}

long sys_socket(const int domain, int type, const int protocol)
{
    const int tflags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

    struct socket* so;
    const int err = sock_create(domain, type, protocol, &so);
    if (err)
        return err;
    return sock_install(so, tflags);
}

long sys_socketpair(const int domain, int type, const int protocol, int* sv)
{
    const int tflags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (domain != AF_UNIX)
        return -EAFNOSUPPORT;
    if (!uaccess_ok_write(sv, 2 * sizeof(int)))
        return -EFAULT;

    struct socket* a;
    struct socket* b;
    const int err = unix_socketpair(type, protocol, &a, &b);
    if (err)
        return err;
    const int fa = sock_install(a, tflags);
    if (fa < 0) {
        sock_close(b);
        return fa;
    }
    const int fb = sock_install(b, tflags);
    if (fb < 0) {
        fd_close(fa);
        return fb;
    }
    const int fds[2] = { fa, fb };
    return copy_to_user(sv, fds, sizeof(fds));
}

long sys_bind(const int fd, const struct sockaddr* addr, const socklen_t len)
{
    struct socket* so;
    struct sockaddr_storage ss;
    long err = sock_lookup(fd, &so);
    if (err || (err = addr_in(&ss, addr, len)))
        return err;
    return sock_bind(so, (struct sockaddr*)&ss, len);
}

long sys_listen(const int fd, const int backlog)
{
    struct socket* so;
    const long err = sock_lookup(fd, &so);
    return err ? err : sock_listen(so, backlog);
}

long sys_connect(const int fd, const struct sockaddr* addr, const socklen_t len)
{
    struct socket* so;
    struct sockaddr_storage ss;
    long err = sock_lookup(fd, &so);
    if (err || (err = addr_in(&ss, addr, len)))
        return err;
    return sock_connect(so, (struct sockaddr*)&ss, len);
}

long sys_accept(const int fd, struct sockaddr* addr, socklen_t* len)
{
    return sys_accept4(fd, addr, len, 0);
}

long sys_accept4(const int fd, struct sockaddr* addr, socklen_t* len, const int flags)
{
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;
    struct socket* so;
    long err = sock_lookup(fd, &so);
    if (err || (err = addr_room(addr, len)))
        return err;

    struct socket* conn;
    struct sockaddr_storage ss;
    socklen_t klen = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    err = sock_accept(so, &conn, (struct sockaddr*)&ss, &klen);
    if (err)
        return err;
    const int nfd = sock_install(conn, flags);
    if (nfd < 0)
        return nfd;
    if (addr && addr_out(&ss, klen, addr, len) < 0) {
        fd_close(nfd);
        return -EFAULT;
    }
    return nfd;
}

long sys_sendto(const int fd, const void* buf, const size_t len, const int flags, const struct sockaddr* dst,
                const socklen_t dst_len)
{
    struct socket* so;
    struct sockaddr_storage ss;
    long err = sock_lookup(fd, &so);
    if (err)
        return err;
    if (!uaccess_ok(buf, len))
        return -EFAULT;
    if (dst && (err = addr_in(&ss, dst, dst_len)))
        return err;
    return sock_send(so, buf, len, flags, dst ? (struct sockaddr*)&ss : nullptr, dst ? dst_len : 0);
}

long sys_recvfrom(const int fd, void* buf, const size_t len, const int flags, struct sockaddr* src,
                  socklen_t* src_len)
{
    struct socket* so;
    long err = sock_lookup(fd, &so);
    if (err || (err = addr_room(src, src_len)))
        return err;
    if (!uaccess_ok_write(buf, len))
        return -EFAULT;

    struct sockaddr_storage ss;
    socklen_t klen = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    const ssize_t n = sock_recv(so, buf, len, flags, src ? (struct sockaddr*)&ss : nullptr, src ? &klen : nullptr);
    if (n >= 0 && src && addr_out(&ss, klen, src, src_len) < 0)
        return -EFAULT;
    return n;
}

static long msg_in(const struct msghdr_s* umsg, struct msghdr_s* msg, struct iovec_s** iov, size_t* total,
                   const bool write)
{
    *iov = nullptr;
    if (copy_from_user(msg, umsg, sizeof(*msg)) < 0)
        return -EFAULT;
    if (msg->msg_iovlen > SOCKET_IOV_MAX)
        return -EMSGSIZE;
    if (msg->msg_iovlen) {
        *iov = malloc(msg->msg_iovlen * sizeof(struct iovec_s));
        if (!*iov)
            return -ENOMEM;
        if (copy_from_user(*iov, msg->msg_iov, msg->msg_iovlen * sizeof(struct iovec_s)) < 0)
            return -EFAULT;
    }
    *total = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        const bool ok = write ? uaccess_ok_write((*iov)[i].iov_base, (*iov)[i].iov_len)
                              : uaccess_ok((*iov)[i].iov_base, (*iov)[i].iov_len);
        if (!ok || (*iov)[i].iov_len > SOCKET_SSIZE_MAX - *total)
            return -EINVAL;
        *total += (*iov)[i].iov_len;
    }
    return 0;
}

static bool sock_is_atomic(const struct socket* so)
{
    return so->proto->flags & PR_ATOMIC;
}

static ssize_t sendmsg_iov(struct socket* so, const struct iovec_s* iov, const size_t iovlen, const size_t total,
                           const int flags, const struct sockaddr* dst, const socklen_t dst_len)
{
    if (iovlen <= 1)
        return sock_send(so, iovlen ? iov[0].iov_base : nullptr, total, flags, dst, dst_len);

    if (sock_is_atomic(so)) {
        if (total > so->snd.hiwat)
            return -EMSGSIZE;
        uint8_t* tmp = malloc(total ? total : 1);
        if (!tmp)
            return -ENOBUFS;
        size_t off = 0;
        for (size_t i = 0; i < iovlen; i++) {
            memcpy(tmp + off, iov[i].iov_base, iov[i].iov_len);
            off += iov[i].iov_len;
        }
        const ssize_t n = sock_send(so, tmp, total, flags, dst, dst_len);
        free(tmp);
        return n;
    }

    size_t sent = 0;
    for (size_t i = 0; i < iovlen; i++) {
        if (!iov[i].iov_len)
            continue;
        const ssize_t n = sock_send(so, iov[i].iov_base, iov[i].iov_len, flags, dst, dst_len);
        if (n < 0)
            return sent ? (ssize_t)sent : n;
        sent += (size_t)n;
        if ((size_t)n < iov[i].iov_len)
            break;
    }
    return (ssize_t)sent;
}

long sys_sendmsg(const int fd, const struct msghdr_s* umsg, const int flags)
{
    struct socket* so;
    long err = sock_lookup(fd, &so);
    if (err)
        return err;
    struct msghdr_s msg;
    struct iovec_s* iov;
    size_t total;
    struct sockaddr_storage ss;
    if ((err = msg_in(umsg, &msg, &iov, &total, false)) == 0 && msg.msg_name)
        err = addr_in(&ss, msg.msg_name, msg.msg_namelen);
    if (err == 0) {
        // TODO: ancillary data (msg_control) is ignored
        err = sendmsg_iov(so, iov, msg.msg_iovlen, total, flags, msg.msg_name ? (struct sockaddr*)&ss : nullptr,
                          msg.msg_name ? msg.msg_namelen : 0);
    }
    free(iov);
    return err;
}

static ssize_t recvmsg_iov(struct socket* so, const struct iovec_s* iov, const size_t iovlen, const size_t total,
                           const int flags, struct sockaddr* from, socklen_t* from_len)
{
    if (iovlen <= 1)
        return sock_recv(so, iovlen ? iov[0].iov_base : nullptr, total, flags, from, from_len);

    if (sock_is_atomic(so)) {
        const size_t cap = total < so->rcv.hiwat ? total : so->rcv.hiwat;
        uint8_t* tmp = malloc(cap ? cap : 1);
        if (!tmp)
            return -ENOBUFS;
        const ssize_t n = sock_recv(so, tmp, cap, flags, from, from_len);
        size_t off = 0;
        for (size_t i = 0; n > 0 && i < iovlen && off < (size_t)n; i++) {
            const size_t c = iov[i].iov_len < (size_t)n - off ? iov[i].iov_len : (size_t)n - off;
            memcpy(iov[i].iov_base, tmp + off, c);
            off += c;
        }
        free(tmp);
        return n;
    }

    size_t got = 0;
    for (size_t i = 0; i < iovlen; i++) {
        if (!iov[i].iov_len)
            continue;
        const int f = got && !(flags & MSG_WAITALL) ? flags | MSG_DONTWAIT : flags;
        const ssize_t n = sock_recv(so, iov[i].iov_base, iov[i].iov_len, f, got ? nullptr : from,
                                    got ? nullptr : from_len);
        if (n < 0)
            return got ? (ssize_t)got : n;
        got += (size_t)n;
        if ((size_t)n < iov[i].iov_len || (flags & MSG_PEEK))
            break;
    }
    return (ssize_t)got;
}

long sys_recvmsg(const int fd, struct msghdr_s* umsg, const int flags)
{
    struct socket* so;
    long err = sock_lookup(fd, &so);
    if (err)
        return err;
    struct msghdr_s msg;
    struct iovec_s* iov;
    size_t total;
    if ((err = msg_in(umsg, &msg, &iov, &total, true)) != 0) {
        free(iov);
        return err;
    }
    if (msg.msg_name && !uaccess_ok_write(msg.msg_name, msg.msg_namelen)) {
        free(iov);
        return -EFAULT;
    }

    struct sockaddr_storage ss;
    socklen_t klen = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    const ssize_t n = recvmsg_iov(so, iov, msg.msg_iovlen, total, flags,
                                  msg.msg_name ? (struct sockaddr*)&ss : nullptr, msg.msg_name ? &klen : nullptr);
    free(iov);
    if (n < 0)
        return n;

    if (msg.msg_name) {
        copy_to_user(msg.msg_name, &ss, klen < msg.msg_namelen ? klen : msg.msg_namelen);
        msg.msg_namelen = klen;
    }
    msg.msg_controllen = 0;
    msg.msg_flags = 0;
    if (copy_to_user(&umsg->msg_namelen, &msg.msg_namelen, sizeof(msg.msg_namelen)) < 0
        || copy_to_user(&umsg->msg_controllen, &msg.msg_controllen, sizeof(msg.msg_controllen)) < 0
        || copy_to_user(&umsg->msg_flags, &msg.msg_flags, sizeof(msg.msg_flags)) < 0)
        return -EFAULT;
    return n;
}

long sys_shutdown(const int fd, const int how)
{
    struct socket* so;
    const long err = sock_lookup(fd, &so);
    return err ? err : sock_shutdown(so, how);
}

static long sys_name(const int fd, struct sockaddr* addr, socklen_t* len, const bool peer)
{
    struct socket* so;
    long err = sock_lookup(fd, &so);
    if (err || (err = addr_room(addr, len)))
        return err;
    if (!addr)
        return -EFAULT;
    struct sockaddr_storage ss;
    socklen_t klen = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    err = peer ? sock_getpeername(so, (struct sockaddr*)&ss, &klen) : sock_getsockname(so, (struct sockaddr*)&ss, &klen);
    if (err)
        return err;
    return addr_out(&ss, klen, addr, len);
}

long sys_getsockname(const int fd, struct sockaddr* addr, socklen_t* len)
{
    return sys_name(fd, addr, len, false);
}

long sys_getpeername(const int fd, struct sockaddr* addr, socklen_t* len)
{
    return sys_name(fd, addr, len, true);
}

long sys_setsockopt(const int fd, const int level, const int optname, const void* optval, const socklen_t optlen)
{
    struct socket* so;
    const long err = sock_lookup(fd, &so);
    if (err)
        return err;
    if (optlen > SOCKOPT_MAX)
        return -EINVAL;
    uint8_t buf[SOCKOPT_MAX];
    if (copy_from_user(buf, optval, optlen) < 0)
        return -EFAULT;
    return sock_setopt(so, level, optname, optlen ? buf : nullptr, optlen);
}

long sys_getsockopt(const int fd, const int level, const int optname, void* optval, socklen_t* optlen)
{
    struct socket* so;
    long err = sock_lookup(fd, &so);
    if (err)
        return err;
    socklen_t room;
    if (copy_from_user(&room, optlen, sizeof(room)) < 0)
        return -EFAULT;
    if ((int32_t)room < 0)
        return -EINVAL;
    uint8_t buf[SOCKOPT_MAX];
    memset(buf, 0, sizeof(buf));
    socklen_t klen = room < SOCKOPT_MAX ? room : SOCKOPT_MAX;
    if ((err = sock_getopt(so, level, optname, buf, &klen)) != 0)
        return err;
    if (klen > room)
        klen = room;
    if (copy_to_user(optval, buf, klen) < 0 || copy_to_user(optlen, &klen, sizeof(klen)) < 0)
        return -EFAULT;
    return 0;
}
