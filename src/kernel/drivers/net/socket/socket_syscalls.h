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

long sys_socket(int domain, int type, int protocol);
long sys_bind(int fd, const struct sockaddr* addr, socklen_t len);
long sys_listen(int fd, int backlog);
long sys_connect(int fd, const struct sockaddr* addr, socklen_t len);
long sys_accept(int fd, struct sockaddr* addr, socklen_t* len);
long sys_sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* dst, socklen_t dst_len);
long sys_recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* src, socklen_t* src_len);
long sys_sendmsg(int fd, const struct msghdr_s* msg, int flags);
long sys_recvmsg(int fd, struct msghdr_s* msg, int flags);
long sys_shutdown(int fd, int how);
long sys_getsockname(int fd, struct sockaddr* addr, socklen_t* len);
long sys_getpeername(int fd, struct sockaddr* addr, socklen_t* len);
long sys_setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen);
long sys_getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen);

#endif //NET_SOCKET_SOCKET_SYSCALLS_H
