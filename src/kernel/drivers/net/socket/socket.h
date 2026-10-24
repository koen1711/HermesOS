#ifndef NET_SOCKET_SOCKET_H
#define NET_SOCKET_SOCKET_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <os/stdbool.h>
#include <os/types.h>
#include <utils/queue/queue.h>
#include <drivers/net/socket/sockaddr.h>
#include <drivers/net/socket/sockbuf.h>

enum so_state
{
    SS_ISCONNECTED = 0x0001,
    SS_ISCONNECTING = 0x0002,
    SS_ISDISCONNECTING = 0x0004,
    SS_NBIO = 0x0100,
    SS_CANTSENDMORE = 0x0010,
    SS_CANTRCVMORE = 0x0020,
};

enum so_options {
    SO_DEBUG = 0x0001,
    SO_ACCEPTCONN = 0x0002,
    SO_REUSEADDR = 0x0004,
    SO_KEEPALIVE = 0x0008,
    SO_BROADCAST = 0x0020,
    SO_REUSEPORT = 0x0200
};

#define SOL_SOCKET 1

enum so_optname
{
    SOOPT_REUSEADDR = 2,
    SOOPT_TYPE = 3,
    SOOPT_ERROR = 4,
    SOOPT_BROADCAST = 6,
    SOOPT_SNDBUF = 7,
    SOOPT_RCVBUF = 8,
    SOOPT_KEEPALIVE = 9,
    SOOPT_REUSEPORT = 15
};


enum shutdown_how
{
    SHUT_RD = 0,
    SHUT_WR = 1,
    SHUT_RDWR = 2
};


// send/recv flags (Linux values, for mlibc)
enum msg_flags
{
    MSG_OOB = 0x01,
    MSG_PEEK = 0x02,
    MSG_DONTWAIT = 0x40,
    MSG_WAITALL = 0x100
};


struct protosw;
struct netbuf;

struct socket {
    int type;
    int16_t state;
    uint32_t options;
    int error;
    int16_t linger;

    struct sockbuf snd;
    struct sockbuf rcv;

    struct protosw* proto;
    void* pcb;

    struct socket* listen_head;
    struct dlist incomplete;
    struct dlist complete;
    struct dlist_node listen_link;
    int backlog;

    int fd;
    uint32_t refcount;

    void (*upcall)(struct socket* so, void* arg);
    void* upcall_arg;
};

void socket_initialize();

int sock_create(int domain, int type, int protocol, struct socket** out);
int sock_bind(struct socket* so, const struct sockaddr* addr, socklen_t len);
int sock_listen(struct socket* so, int backlog);
int sock_connect(struct socket* so, const struct sockaddr* addr, socklen_t len);
int sock_accept(struct socket* so, struct socket** out, struct sockaddr* addr, socklen_t* len);
ssize_t sock_send(struct socket* so, const void* buf, size_t len, int flags,
                  const struct sockaddr* dst, socklen_t dst_len);
ssize_t sock_recv(struct socket* so, void* buf, size_t len, int flags,
                  struct sockaddr* from, socklen_t* from_len);
int sock_shutdown(struct socket* so, int how);
int sock_close(struct socket* so);
int sock_setopt(struct socket* so, int level, int optname, const void* optval, socklen_t optlen);
int sock_getopt(struct socket* so, int level, int optname, void* optval, socklen_t* optlen);
int sock_getsockname(struct socket* so, struct sockaddr* addr, socklen_t* len);
int sock_getpeername(struct socket* so, struct sockaddr* addr, socklen_t* len);

void sock_is_connecting(struct socket* so);
void sock_is_connected(struct socket* so);
void sock_is_disconnecting(struct socket* so);
void sock_is_disconnected(struct socket* so);
void sock_cant_send_more(struct socket* so);
void sock_cant_recv_more(struct socket* so);

struct socket* sock_new_conn(struct socket* head);

void sock_release(struct socket* so);
void sock_wakeup(struct socket* so);
void sock_set_upcall(struct socket* so, void (*upcall)(struct socket* so, void* arg), void* arg);

#endif //NET_SOCKET_SOCKET_H
