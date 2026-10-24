#include <drivers/net/tcp/tcp_usrreq.h>

#include <drivers/net/tcp/tcp_output.h>
#include <drivers/net/tcp/tcp_subr.h>
#include <drivers/net/tcp/tcp_timer.h>
#include <drivers/net/inpcb/inpcb.h>
#include <drivers/net/socket/protosw.h>
#include <drivers/net/socket/socket.h>
#include <drivers/net/core/netbuf.h>
#include <os/errno.h>

#define TCP_PROTOSW(dom) {                      \
    .type = SOCK_STREAM,                        \
    .domain = (dom),                            \
    .protocol = IPPROTO_TCP,                    \
    .flags = PR_CONNREQUIRED,                   \
    .pr_attach = tcp_attach,                    \
    .pr_detach = tcp_detach,                    \
    .pr_bind = tcp_bind,                        \
    .pr_listen = tcp_listen,                    \
    .pr_connect = tcp_connect,                  \
    .pr_accept = tcp_accept,                    \
    .pr_disconnect = tcp_disconnect,            \
    .pr_send = tcp_send,                        \
    .pr_rcvd = tcp_rcvd,                        \
    .pr_shutdown = tcp_shutdown,                \
    .pr_abort = tcp_abort,                      \
    .pr_ctloutput = tcp_ctloutput,              \
    .pr_sockaddr = inpcb_pr_sockaddr,           \
    .pr_peeraddr = inpcb_pr_peeraddr,           \
}

struct protosw tcp_protosw4 = TCP_PROTOSW(AF_INET);
struct protosw tcp_protosw6 = TCP_PROTOSW(AF_INET6);

static struct tcpcb* tcp_tcpcb(const struct socket* so)
{
    const struct inpcb* inp = so->pcb;
    return inp ? inp->ppcb : nullptr;
}

static int tcp_gone_error(const struct socket* so)
{
    return so->error ? -so->error : -ENOTCONN;
}

int tcp_attach(struct socket* so, const int proto)
{
    (void)proto;
    uint64_t flags;
    tcp_lock(&flags);
    const int err = tcp_attach_pcb(so);
    tcp_unlock(flags);
    return err;
}

int tcp_bind(struct socket* so, const struct sockaddr* addr, const socklen_t len)
{
    uint64_t flags;
    tcp_lock(&flags);
    const int err = so->pcb ? inpcb_bind(so->pcb, addr, len) : -EINVAL;
    tcp_unlock(flags);
    return err;
}

int tcp_listen(struct socket* so, const int backlog)
{
    (void)backlog;
    uint64_t flags;
    tcp_lock(&flags);
    struct tcpcb* tp = tcp_tcpcb(so);
    int err = 0;
    if (!tp || (tp->state != TCPS_CLOSED && tp->state != TCPS_LISTEN))
        err = -EINVAL;
    else if (!tp->inp->lport)
        err = inpcb_bind(tp->inp, nullptr, 0);
    if (!err)
        tcp_state_change(tp, TCPS_LISTEN);
    tcp_unlock(flags);
    return err;
}

static int tcp_connect_locked(struct socket* so, const struct sockaddr* addr, const socklen_t len)
{
    struct tcpcb* tp = tcp_tcpcb(so);
    if (!tp)
        return -EINVAL;
    if (tp->state == TCPS_SYN_SENT)
        return -EALREADY;
    if (tp->state != TCPS_CLOSED)
        return -EISCONN;

    const int err = inpcb_connect(tp->inp, addr, len);
    if (err)
        return err;

    tcp_sendseqinit(tp);
    tcp_state_change(tp, TCPS_SYN_SENT);
    sock_is_connecting(so);
    tcp_timer_activate(tp, TCPT_KEEP, TCP_KEEPINIT_MS);
    tcpstat.connattempt++;
    tcp_output(tp);
    return 0;
}

int tcp_connect(struct socket* so, const struct sockaddr* addr, const socklen_t len)
{
    uint64_t flags;
    tcp_lock(&flags);
    const int err = tcp_connect_locked(so, addr, len);
    tcp_unlock(flags);
    return err;
}

int tcp_accept(struct socket* so, struct sockaddr* addr, socklen_t* len)
{
    if (!so->pcb)
        return -ECONNABORTED;
    if (addr && len)
        return inpcb_pr_peeraddr(so, addr, len);
    return 0;
}

int tcp_send(struct socket* so, struct netbuf* nb, const struct sockaddr* dst, socklen_t dst_len, const int flags)
{
    (void)dst;
    (void)dst_len;
    if (flags & MSG_OOB) {
        netbuf_free_chain(nb);
        return -EOPNOTSUPP;
    }

    uint64_t lock_flags;
    tcp_lock(&lock_flags);
    struct tcpcb* tp = tcp_tcpcb(so);
    int err = 0;
    if (!tp)
        err = tcp_gone_error(so);
    else if (tp->state < TCPS_SYN_SENT)
        err = -ENOTCONN;
    else if (tp->state > TCPS_CLOSE_WAIT)
        err = -EPIPE;
    if (err) {
        netbuf_free_chain(nb);
    } else {
        sockbuf_append(&so->snd, nb);
        tcp_output(tp);
    }
    tcp_unlock(lock_flags);
    return err;
}

int tcp_rcvd(struct socket* so, const int flags)
{
    (void)flags;
    uint64_t lock_flags;
    tcp_lock(&lock_flags);
    struct tcpcb* tp = tcp_tcpcb(so);
    if (tp)
        tcp_output(tp);
    tcp_unlock(lock_flags);
    return 0;
}

int tcp_shutdown(struct socket* so, const int how)
{
    if (how == SHUT_RD)
        return 0;
    uint64_t flags;
    tcp_lock(&flags);
    struct tcpcb* tp = tcp_tcpcb(so);
    sock_cant_send_more(so);
    if (tp && (tp = tcp_usrclosed(tp)))
        tcp_output(tp);
    tcp_unlock(flags);
    return 0;
}

static void tcp_disconnect_tp(struct socket* so, struct tcpcb* tp)
{
    if (!TCPS_HAVEESTABLISHED(tp->state)) {
        tcp_close(tp);
    } else if (so->linger == 0) {
        tcp_drop(tp, 0);
    } else {
        sock_is_disconnecting(so);
        if ((tp = tcp_usrclosed(tp)))
            tcp_output(tp);
    }
}

int tcp_disconnect(struct socket* so)
{
    uint64_t flags;
    tcp_lock(&flags);
    struct tcpcb* tp = tcp_tcpcb(so);
    if (tp)
        tcp_disconnect_tp(so, tp);
    tcp_unlock(flags);
    return tp ? 0 : -ENOTCONN;
}

void tcp_abort(struct socket* so)
{
    uint64_t flags;
    tcp_lock(&flags);
    struct tcpcb* tp = tcp_tcpcb(so);
    if (tp)
        tcp_drop(tp, ECONNABORTED);
    tcp_unlock(flags);
}

void tcp_detach(struct socket* so)
{
    uint64_t flags;
    tcp_lock(&flags);
    struct inpcb* inp = so->pcb;
    if (inp) {
        inp->flags |= INPCB_DETACHED;
        struct tcpcb* tp = inp->ppcb;
        if (!tp)
            tcp_pcb_release(inp);
        else if (!TCPS_HAVEESTABLISHED(tp->state))
            tcp_close(tp);
        else if ((tp = tcp_usrclosed(tp)))
            tcp_output(tp);
    }
    tcp_unlock(flags);
}

static int tcp_ctloutput_int(void* optval, socklen_t* optlen, int* value, const bool set)
{
    if (*optlen < sizeof(int))
        return -EINVAL;
    if (set)
        *value = *(const int*)optval;
    else
        *(int*)optval = *value;
    *optlen = sizeof(int);
    return 0;
}

int tcp_ctloutput(struct socket* so, const int level, const int optname, void* optval, socklen_t* optlen, const bool set)
{
    if (level != IPPROTO_TCP)
        return -ENOPROTOOPT;

    uint64_t flags;
    tcp_lock(&flags);
    struct tcpcb* tp = tcp_tcpcb(so);
    int err = -ENOPROTOOPT;
    int value;
    if (!tp) {
        err = tcp_gone_error(so);
    } else if (optname == TCP_NODELAY) {
        value = !!(tp->t_flags & TF_NODELAY);
        err = tcp_ctloutput_int(optval, optlen, &value, set);
        if (!err && set)
            tp->t_flags = value ? tp->t_flags | TF_NODELAY : tp->t_flags & ~TF_NODELAY;
        if (!err && set && value)
            tcp_output(tp);
    } else if (optname == TCP_MAXSEG) {
        value = tp->mss;
        err = tcp_ctloutput_int(optval, optlen, &value, set);
        if (!err && set && (value < 64 || value > tp->mss))
            err = -EINVAL;
        else if (!err && set)
            tp->mss = value;
    }
    tcp_unlock(flags);
    return err;
}
