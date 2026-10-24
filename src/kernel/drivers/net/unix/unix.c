#include "unix.h"

#include <drivers/net/socket/socket.h>
#include <drivers/net/core/netbuf.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

static struct socket* peer_of(const struct socket* so)
{
    return so->pcb;
}

static int unix_attach(struct socket* so, const int proto)
{
    (void)proto;
    so->pcb = nullptr;
    return 0;
}

static void unix_detach(struct socket* so)
{
    struct socket* peer = peer_of(so);
    so->pcb = nullptr;
    if (!peer)
        return;
    peer->pcb = nullptr;
    sock_cant_recv_more(peer);
    sock_cant_send_more(peer);
}

static int unix_send(struct socket* so, struct netbuf* nb, const struct sockaddr* dst, const socklen_t dst_len,
                     const int flags)
{
    (void)dst;
    (void)dst_len;
    (void)flags;
    struct socket* peer = peer_of(so);
    if (!peer || (peer->state & SS_CANTRCVMORE)) {
        netbuf_free_chain(nb);
        return -EPIPE;
    }
    sockbuf_append(&peer->rcv, nb);
    so->snd.cc = peer->rcv.cc;
    sock_wakeup(peer);
    return 0;
}

static int unix_rcvd(struct socket* so, const int flags)
{
    (void)flags;
    struct socket* peer = peer_of(so);
    if (peer) {
        peer->snd.cc = so->rcv.cc;
        sock_wakeup(peer);
    }
    return 0;
}

static int unix_shutdown(struct socket* so, const int how)
{
    (void)how;
    sock_cant_send_more(so);
    struct socket* peer = peer_of(so);
    if (peer)
        sock_cant_recv_more(peer);
    return 0;
}

static int unix_name(struct socket* so, struct sockaddr* addr, socklen_t* len)
{
    (void)so;
    const uint16_t family = AF_UNIX;
    const socklen_t n = *len < sizeof(family) ? *len : sizeof(family);
    memcpy(addr, &family, n);
    *len = sizeof(family);
    return 0;
}

struct protosw unix_stream_protosw = {
    .type = SOCK_STREAM,
    .domain = AF_UNIX,
    .protocol = 0,
    .flags = PR_CONNREQUIRED,
    .pr_attach = unix_attach,
    .pr_detach = unix_detach,
    .pr_send = unix_send,
    .pr_rcvd = unix_rcvd,
    .pr_shutdown = unix_shutdown,
    .pr_sockaddr = unix_name,
    .pr_peeraddr = unix_name,
};

int unix_socketpair(const int type, const int protocol, struct socket** a, struct socket** b)
{
    *a = *b = nullptr;
    if (type != SOCK_STREAM)
        return -EOPNOTSUPP;
    int err = sock_create(AF_UNIX, type, protocol, a);
    if (err)
        return err;
    err = sock_create(AF_UNIX, type, protocol, b);
    if (err) {
        sock_close(*a);
        *a = nullptr;
        return err;
    }
    (*a)->pcb = *b;
    (*b)->pcb = *a;
    sock_is_connected(*a);
    sock_is_connected(*b);
    return 0;
}
