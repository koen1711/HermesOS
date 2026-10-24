#ifndef NET_SOCKET_PROTOSW_H
#define NET_SOCKET_PROTOSW_H

#include <os/stdint.h>
#include <drivers/net/socket/sockaddr.h>

struct socket;
struct netbuf;

struct protosw {
    int type;
    int domain;
    int protocol;
    uint32_t flags;

    int (*pr_attach)(struct socket* so, int proto);
    void (*pr_detach)(struct socket* so);
    int (*pr_bind)(struct socket* so, const struct sockaddr* addr, socklen_t len);
    int (*pr_listen)(struct socket* so, int backlog);
    int (*pr_connect)(struct socket* so, const struct sockaddr* addr, socklen_t len);
    int (*pr_accept)(struct socket* so, struct sockaddr* addr, socklen_t* len);
    int (*pr_disconnect)(struct socket* so);
    int (*pr_send)(struct socket* so, struct netbuf* nb, const struct sockaddr* dst, socklen_t dst_len, int flags);
    int (*pr_rcvd)(struct socket* so, int flags);
    int (*pr_shutdown)(struct socket* so, int how);
    void (*pr_abort)(struct socket* so);
    int (*pr_ctloutput)(struct socket* so, int level, int optname, void* optval, socklen_t* optlen, bool set);
    int (*pr_sockaddr)(struct socket* so, struct sockaddr* addr, socklen_t* len);
    int (*pr_peeraddr)(struct socket* so, struct sockaddr* addr, socklen_t* len);
};

enum pr_flags
{
    PR_ATOMIC = 0x01,
    PR_ADDR = 0x02,
    PR_CONNREQUIRED = 0x04
};

struct domain {
    int family;
    const char* name;
    struct protosw* protosw;
    uint32_t protosw_count;
};

void protosw_initialize();
int domain_register(struct domain* dom);
int protosw_register(struct domain* dom, struct protosw* pr);

struct protosw* protosw_find(int domain, int type, int protocol);

#endif //NET_SOCKET_PROTOSW_H
