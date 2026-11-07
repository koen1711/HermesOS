#ifndef NET_SOCKET_SOCKET_SYSCALLS_H
#define NET_SOCKET_SOCKET_SYSCALLS_H

#include <os/types.h>
#include <os/stddef.h>
#include <drivers/net/socket/sockaddr.h>

enum socket_syscall_numbers {
    SYSCALL_SOCKET = 41,
    SYSCALL_CONNECT = 42,
    SYSCALL_ACCEPT = 43,
    SYSCALL_SENDTO = 44,
    SYSCALL_RECVFROM = 45,
    SYSCALL_SENDMSG = 46,
    SYSCALL_RECVMSG = 47,
    SYSCALL_SHUTDOWN = 48,
    SYSCALL_BIND = 49,
    SYSCALL_LISTEN = 50,
    SYSCALL_GETSOCKNAME = 51,
    SYSCALL_GETPEERNAME = 52,
    SYSCALL_SOCKETPAIR = 53,
    SYSCALL_SETSOCKOPT = 54,
    SYSCALL_GETSOCKOPT = 55,
    SYSCALL_ACCEPT4 = 288,
};

enum socket_type_flags {
    SOCK_NONBLOCK = 0x800,
    SOCK_CLOEXEC = 0x80000
};

#define SOCKET_IOV_MAX 1024

struct iovec_s {
    void* iov_base;
    size_t iov_len;
};

struct msghdr_s {
    void* msg_name;
    socklen_t msg_namelen;
    struct iovec_s* msg_iov;
    size_t msg_iovlen;
    void* msg_control;
    size_t msg_controllen;
    int msg_flags;
};

struct socket;

long sys_socket(int domain, int type, int protocol);
long sys_bind(int fd, const struct sockaddr* addr, socklen_t len);
long sys_listen(int fd, int backlog);
long sys_connect(int fd, const struct sockaddr* addr, socklen_t len);
long sys_accept(struct socket* so, struct sockaddr* addr, socklen_t* len);
long sys_accept4(struct socket* so, struct sockaddr* addr, socklen_t* len, int flags);
long sys_socketpair(int domain, int type, int protocol, int* sv);
long sys_sendto(struct socket* so, const void* buf, size_t len, int flags, const struct sockaddr* dst, socklen_t dst_len);
long sys_recvfrom(struct socket* so, void* buf, size_t len, int flags, struct sockaddr* src, socklen_t* src_len);
long sys_sendmsg(struct socket* so, const struct msghdr_s* msg, int flags);
long sys_recvmsg(struct socket* so, struct msghdr_s* msg, int flags);
long sys_shutdown(struct socket* so, int how);
long sys_getsockname(struct socket* so, struct sockaddr* addr, socklen_t* len);
long sys_getpeername(struct socket* so, struct sockaddr* addr, socklen_t* len);
long sys_setsockopt(struct socket* so, int level, int optname, const void* optval, socklen_t optlen);
long sys_getsockopt(struct socket* so, int level, int optname, void* optval, socklen_t* optlen);

#endif //NET_SOCKET_SOCKET_SYSCALLS_H
