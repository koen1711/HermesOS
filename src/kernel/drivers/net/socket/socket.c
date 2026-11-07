#include <drivers/net/socket/socket.h>

#include <drivers/net/socket/protosw.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/timer/callout.h>
#include <drivers/scheduler/process.h>
#include <drivers/terminal/terminal.h>
#include <hardware/memory/alloc.h>
#include <hardware/smp/smp.h>
#include <utils/sync/spinlock.h>
#include <os/errno.h>

#define SOCKET_MAX_BUFFER (4 * 1024 * 1024)

static spinlock_t accept_lock = { .name = "so_accept" };

static struct {
    struct socket* held[SOCK_LOCK_NEST];
    int n;
} __attribute__((aligned(64))) held_by_cpu[MAX_CPUS];

void socket_initialize()
{
}

static void sock_lock_spin(struct sock_lock* l)
{
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) {
            smp_spin_poll();
            asm volatile("pause");
        }
    }
}

static void sock_lock_halt(const char* what, const struct socket* so)
{
    terminal_printf("\nsocket lock: %s (so=0x%lx)\n", what, (uint64_t)so);
    while (1)
        asm volatile("cli; hlt");
}

static void held_push(const int cpu, struct socket* so)
{
    if (held_by_cpu[cpu].n >= SOCK_LOCK_NEST)
        sock_lock_halt("nesting too deep", so);
    held_by_cpu[cpu].held[held_by_cpu[cpu].n++] = so;
}

static void held_remove(const int cpu, const struct socket* so)
{
    const int n = held_by_cpu[cpu].n;
    for (int i = n - 1; i >= 0; i--) {
        if (held_by_cpu[cpu].held[i] == so) {
            for (int j = i; j < n - 1; j++)
                held_by_cpu[cpu].held[j] = held_by_cpu[cpu].held[j + 1];
            held_by_cpu[cpu].n = n - 1;
            return;
        }
    }
    sock_lock_halt("unlock of a lock this CPU does not hold", so);
}

void sock_lock(struct socket* so)
{
    const int cpu = cpu_this()->index;
    if (so->lock.owner == cpu) {
        so->lock.depth++;
        return;
    }
    sock_lock_spin(&so->lock);
    so->lock.owner = cpu;
    so->lock.depth = 1;
    held_push(cpu, so);
}

void sock_unlock(struct socket* so)
{
    const int cpu = cpu_this()->index;
    if (so->lock.owner != cpu || so->lock.depth == 0)
        sock_lock_halt("unlock of a lock this CPU does not hold", so);
    if (--so->lock.depth)
        return;
    held_remove(cpu, so);
    so->lock.owner = -1;
    __atomic_store_n(&so->lock.locked, 0, __ATOMIC_RELEASE);
}

bool sock_lock_held(const struct socket* so)
{
    return so->lock.owner == cpu_this()->index;
}

bool sock_lock_any_held()
{
    return held_by_cpu[cpu_this()->index].n != 0;
}

void sock_lock_save(struct sock_lock_saved* saved)
{
    const int cpu = cpu_this()->index;
    saved->n = held_by_cpu[cpu].n;
    for (int i = 0; i < saved->n; i++) {
        struct socket* so = held_by_cpu[cpu].held[i];
        saved->so[i] = so;
        saved->depth[i] = so->lock.depth;
        so->lock.depth = 0;
        so->lock.owner = -1;
        __atomic_store_n(&so->lock.locked, 0, __ATOMIC_RELEASE);
    }
    held_by_cpu[cpu].n = 0;
}

void sock_lock_restore(const struct sock_lock_saved* saved)
{
    const int cpu = cpu_this()->index;
    for (int i = 0; i < saved->n; i++) {
        struct socket* so = saved->so[i];
        sock_lock_spin(&so->lock);
        so->lock.owner = cpu;
        so->lock.depth = saved->depth[i];
        held_push(cpu, so);
    }
}

static bool sock_nonblocking(const struct socket* so, const int flags)
{
    return (so->state & SS_NBIO) || (flags & MSG_DONTWAIT);
}

static int sock_take_error(struct socket* so)
{
    const int err = so->error;
    so->error = 0;
    return err;
}

static struct socket* sock_alloc(const int type, struct protosw* pr)
{
    struct socket* so = malloc_safe(sizeof(struct socket));
    if (!so)
        return nullptr;

    so->lock.owner = -1;
    so->type = type;
    so->proto = pr;
    so->fd = -1;
    so->refcount = 1;
    so->linger = -1;
    sockbuf_init(&so->snd, SOCKBUF_DEFAULT_SIZE);
    sockbuf_init(&so->rcv, SOCKBUF_DEFAULT_SIZE);
    dlist_init(&so->incomplete);
    dlist_init(&so->complete);
    return so;
}

static void sock_free(struct socket* so)
{
    sockbuf_release(&so->snd);
    sockbuf_release(&so->rcv);
    free(so);
}

int sock_create(const int domain, const int type, const int protocol, struct socket** out)
{
    *out = nullptr;
    if (domain != AF_INET && domain != AF_INET6 && domain != AF_UNIX)
        return -EAFNOSUPPORT;
    struct protosw* pr = protosw_find(domain, type, protocol);
    if (!pr)
        return protosw_find(domain, type, 0) ? -EPROTONOSUPPORT : -ESOCKTNOSUPPORT;
    if (!pr->pr_attach)
        return -EPROTONOSUPPORT;

    struct socket* so = sock_alloc(type, pr);
    if (!so)
        return -ENOMEM;
    const int err = pr->pr_attach(so, protocol);
    if (err) {
        sock_free(so);
        return err;
    }
    *out = so;
    return 0;
}

static int sock_bind_locked(struct socket* so, const struct sockaddr* addr, const socklen_t len)
{
    if (!addr)
        return -EINVAL;
    if (!so->proto->pr_bind)
        return -EOPNOTSUPP;
    return so->proto->pr_bind(so, addr, len);
}

static int sock_listen_locked(struct socket* so, int backlog)
{
    if (!so->proto->pr_listen)
        return -EOPNOTSUPP;
    if (so->state & (SS_ISCONNECTED | SS_ISCONNECTING))
        return -EINVAL;
    if (backlog < 1)
        backlog = 1;
    if (backlog > 128)
        backlog = 128;
    const int err = so->proto->pr_listen(so, backlog);
    if (err)
        return err;
    so->backlog = backlog;
    so->options |= SO_ACCEPTCONN;
    return 0;
}

static int sock_wait(struct sockbuf* sb, const uint64_t timeo, const uint64_t start)
{
    if (!timeo)
        return sockbuf_wait(sb);

    const uint64_t waited = ticks_get() - start;
    if (waited > timeo)
        return -EAGAIN;
    return sockbuf_timedwait(sb, timeo + 1 - waited);
}

#define SOCK_WAIT_UNTIL(so, sb, flags, timeo, cond)                 \
    for (const uint64_t start_ = ticks_get(); !(cond);) {          \
        if ((so)->error)                                           \
            return -sock_take_error(so);                           \
        if (sock_nonblocking(so, flags))                           \
            return -EAGAIN;                                        \
        const int werr_ = sock_wait(sb, timeo, start_);            \
        if (werr_)                                                 \
            return werr_;                                          \
    }

static int sock_connect_locked(struct socket* so, const struct sockaddr* addr, const socklen_t len)
{
    if (!addr)
        return -EINVAL;
    if (so->options & SO_ACCEPTCONN)
        return -EOPNOTSUPP;
    if (!so->proto->pr_connect)
        return -EOPNOTSUPP;
    const bool stream = so->proto->flags & PR_CONNREQUIRED;
    if (stream && (so->state & SS_ISCONNECTED))
        return -EISCONN;
    if (stream && (so->state & SS_ISCONNECTING))
        return -EALREADY;

    so->error = 0;
    const int err = so->proto->pr_connect(so, addr, len);
    if (err)
        return err;
    if (!stream)
        return 0;

    if (so->state & SS_ISCONNECTING) {
        if (so->state & SS_NBIO)
            return -EINPROGRESS;

        const uint64_t start = ticks_get();
        while ((so->state & SS_ISCONNECTING) && !so->error) {
            const int werr = sock_wait(&so->rcv, so->snd_timeo, start);

            if (werr)
                return werr == -EAGAIN ? -EINPROGRESS : werr == -ERESTARTSYS ? -EINTR : werr;
        }
    }
    if (so->error)
        return -sock_take_error(so);
    return (so->state & SS_ISCONNECTED) ? 0 : -ECONNREFUSED;
}

static int sock_accept_locked(struct socket* so, struct socket** out, struct sockaddr* addr, socklen_t* len)
{
    *out = nullptr;
    if (!(so->options & SO_ACCEPTCONN))
        return -EINVAL;
    SOCK_WAIT_UNTIL(so, &so->rcv, 0, so->rcv_timeo, !dlist_empty(&so->complete));

    uint64_t flags;
    spinlock_acquire_irqsave(&accept_lock, &flags);
    struct socket* conn = dlist_entry(dlist_first(&so->complete), struct socket, listen_link);
    dlist_remove(&so->complete, &conn->listen_link);
    conn->listen_head = nullptr;
    spinlock_release_irqrestore(&accept_lock, flags);

    sock_lock(conn);
    int err = 0;
    if (so->proto->pr_accept)
        err = so->proto->pr_accept(conn, addr, len);
    else if (addr && len && so->proto->pr_peeraddr)
        so->proto->pr_peeraddr(conn, addr, len);
    sock_unlock(conn);
    if (err) {
        sock_close(conn);
        return err;
    }
    *out = conn;
    return 0;
}

static struct netbuf* sock_make_netbuf(const void* buf, const size_t len)
{
    struct netbuf* nb = netbuf_alloc(true);
    if (!nb)
        return nullptr;
    if (len && !netbuf_append(nb, buf, len)) {
        netbuf_free_chain(nb);
        return nullptr;
    }
    return nb;
}

static ssize_t sock_send_locked(struct socket* so, const void* buf, const size_t len, const int flags,
                                const struct sockaddr* dst, const socklen_t dst_len)
{
    if (so->state & SS_CANTSENDMORE)
        return -EPIPE;
    if (so->error)
        return -sock_take_error(so);
    if (!so->proto->pr_send)
        return -EOPNOTSUPP;

    const bool stream = so->proto->flags & PR_CONNREQUIRED;
    if (stream && !(so->state & SS_ISCONNECTED))
        return -ENOTCONN;

    if (so->proto->flags & PR_ATOMIC) {
        if (len > so->snd.hiwat)
            return -EMSGSIZE;
        struct netbuf* nb = sock_make_netbuf(buf, len);
        if (!nb)
            return -ENOBUFS;
        const int err = so->proto->pr_send(so, nb, dst, dst_len, flags);
        return err < 0 ? err : (ssize_t)len;
    }

    const uint8_t* p = buf;
    size_t sent = 0;
    const uint64_t start = ticks_get();
    while (sent < len) {
        if (so->state & SS_CANTSENDMORE)
            return sent ? (ssize_t)sent : -EPIPE;
        if (so->error)
            return sent ? (ssize_t)sent : -sock_take_error(so);
        const size_t space = sockbuf_space(&so->snd);
        if (space == 0) {
            if (sock_nonblocking(so, flags))
                return sent ? (ssize_t)sent : -EAGAIN;
            const int werr = sock_wait(&so->snd, so->snd_timeo, start);
            if (werr)
                return sent ? (ssize_t)sent : werr;
            continue;
        }
        const size_t chunk = len - sent < space ? len - sent : space;
        struct netbuf* nb = sock_make_netbuf(p + sent, chunk);
        if (!nb)
            return sent ? (ssize_t)sent : -ENOBUFS;
        const int err = so->proto->pr_send(so, nb, dst, dst_len, flags);
        if (err < 0)
            return sent ? (ssize_t)sent : err;
        sent += chunk;
    }
    return (ssize_t)sent;
}

struct netbuf* sock_send_prebuild(const struct socket* so, const void* buf, const size_t len)
{
    const uint32_t flags = so->proto->flags;
    if (!(flags & PR_CONNREQUIRED) || (flags & PR_ATOMIC) || !len || len > sockbuf_space(&so->snd))
        return nullptr;
    return sock_make_netbuf(buf, len);
}

static ssize_t sock_send_prebuilt_locked(struct socket* so, struct netbuf* nb, const void* buf, const size_t len)
{
    if ((so->state & SS_CANTSENDMORE) || so->error || !(so->state & SS_ISCONNECTED)
        || !so->proto->pr_send || sockbuf_space(&so->snd) < len) {
        netbuf_free_chain(nb);
        return sock_send_locked(so, buf, len, 0, nullptr, 0);
    }
    const int err = so->proto->pr_send(so, nb, nullptr, 0, 0);
    return err < 0 ? err : (ssize_t)len;
}

static const struct netbuf* sock_record_addr(const struct netbuf* rec)
{
    return rec && rec->flags & NETBUF_SOCKADDR ? rec : nullptr;
}

static ssize_t sock_recv_locked(struct socket* so, void* buf, const size_t len, const int flags,
                                struct sockaddr* from, socklen_t* from_len)
{
    const uint64_t start = ticks_get();
    while (!so->rcv.head) {
        if (so->error)
            return -sock_take_error(so);
        if (so->state & SS_CANTRCVMORE)
            return 0;
        if (so->proto->flags & PR_CONNREQUIRED && !(so->state & (SS_ISCONNECTED | SS_ISCONNECTING)))
            return -ENOTCONN;
        if (sock_nonblocking(so, flags))
            return -EAGAIN;
        const int werr = sock_wait(&so->rcv, so->rcv_timeo, start);
        if (werr)
            return werr;
    }

    if (so->proto->flags & PR_ATOMIC) {
        const struct netbuf* rec = so->rcv.head;
        const struct netbuf* addr = sock_record_addr(rec);
        if (from && from_len) {
            socklen_t n = addr ? (socklen_t)addr->len : 0;
            if (n > *from_len)
                n = *from_len;
            if (n)
                memcpy(from, addr->data, n);
            *from_len = addr ? (socklen_t)addr->len : 0;
        }
        size_t copied = 0;
        uint8_t* dst = buf;
        for (const struct netbuf* b = addr ? rec->next : rec; b && copied < len; b = b->next) {
            const size_t n = b->len < len - copied ? b->len : len - copied;
            memcpy(dst + copied, b->data, n);
            copied += n;
        }
        if (!(flags & MSG_PEEK)) {
            sockbuf_drop_record(&so->rcv);
            if (so->proto->pr_rcvd)
                so->proto->pr_rcvd(so, flags);
        }
        return (ssize_t)copied;
    }

    const size_t want = len;
    if ((flags & MSG_WAITALL) && !(flags & MSG_PEEK)) {
        while (so->rcv.cc < want && !(so->state & SS_CANTRCVMORE) && !so->error && !sock_nonblocking(so, flags))
            if (sock_wait(&so->rcv, so->rcv_timeo, start))
                break;
    }
    const size_t n = so->rcv.cc < want ? so->rcv.cc : want;
    sockbuf_copy(&so->rcv, 0, n, buf);
    if (!(flags & MSG_PEEK)) {
        sockbuf_drop(&so->rcv, n);
        if (so->proto->pr_rcvd)
            so->proto->pr_rcvd(so, flags);
    }
    if (from && from_len && so->proto->pr_peeraddr)
        so->proto->pr_peeraddr(so, from, from_len);
    return (ssize_t)n;
}

static int sock_shutdown_locked(struct socket* so, const int how)
{
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
        return -EINVAL;
    if ((so->proto->flags & PR_CONNREQUIRED) && !(so->state & (SS_ISCONNECTED | SS_ISCONNECTING)))
        return -ENOTCONN;
    if (how != SHUT_WR) {
        sock_cant_recv_more(so);
        sockbuf_release(&so->rcv);
    }
    if (how != SHUT_RD && so->proto->pr_shutdown)
        return so->proto->pr_shutdown(so, how);
    return 0;
}

static int sock_close_locked(struct socket* so)
{
    for (;;) {
        uint64_t flags;
        spinlock_acquire_irqsave(&accept_lock, &flags);
        if (dlist_empty(&so->incomplete) && dlist_empty(&so->complete)) {
            spinlock_release_irqrestore(&accept_lock, flags);
            break;
        }
        struct dlist* q = dlist_empty(&so->incomplete) ? &so->complete : &so->incomplete;
        const auto conn = dlist_entry(dlist_first(q), struct socket, listen_link);
        dlist_remove(q, &conn->listen_link);
        conn->listen_head = nullptr;
        spinlock_release_irqrestore(&accept_lock, flags);
        if (conn->proto->pr_abort) {
            sock_hold(conn);
            sock_lock(conn);
            conn->proto->pr_abort(conn);
            sock_unlock(conn);
            sock_release(conn);
        } else {
            sock_close(conn);
        }
    }

    if (so->state & SS_ISCONNECTED && (so->proto->flags & PR_CONNREQUIRED) && so->proto->pr_disconnect)
        so->proto->pr_disconnect(so);
    if (so->proto->pr_detach)
        so->proto->pr_detach(so);
    sock_release(so);
    return 0;
}

static int sock_timeval_to_ticks(const struct sock_timeval* tv, uint64_t* out)
{
    if (tv->tv_usec < 0 || tv->tv_usec >= 1000000)
        return -EDOM;
    if (tv->tv_sec < 0) {
        *out = 1;
        return 0;
    }
    const uint64_t hz = ticks_hz();
    const uint64_t sec = tv->tv_sec > 1000000000000 ? 1000000000000 : (uint64_t)tv->tv_sec;
    *out = sec * hz + ((uint64_t)tv->tv_usec * hz + 999999) / 1000000;
    return 0;
}

static int sock_setopt_locked(struct socket* so, const int level, const int optname, const void* optval, const socklen_t optlen)
{
    if (level != SOL_SOCKET) {
        if (!so->proto->pr_ctloutput)
            return -ENOPROTOOPT;
        socklen_t l = optlen;
        return so->proto->pr_ctloutput(so, level, optname, (void*)optval, &l, true);
    }

    if (optname == SOOPT_RCVTIMEO || optname == SOOPT_SNDTIMEO) {
        if (!optval || optlen < sizeof(struct sock_timeval))
            return -EINVAL;
        const auto tv = (const struct sock_timeval*)optval;
        const int err = sock_timeval_to_ticks(tv, optname == SOOPT_RCVTIMEO ? &so->rcv_timeo : &so->snd_timeo);
        return err;
    }

    if (!optval || optlen < sizeof(int))
        return -EINVAL;
    const int v = *(const int*)optval;

    uint32_t bit = 0;
    switch (optname) {
    case SOOPT_REUSEADDR: bit = SO_REUSEADDR; break;
    case SOOPT_REUSEPORT: bit = SO_REUSEPORT; break;
    case SOOPT_BROADCAST: bit = SO_BROADCAST; break;
    case SOOPT_KEEPALIVE: bit = SO_KEEPALIVE; break;
    case SOOPT_SNDBUF:
    case SOOPT_RCVBUF: {
        if (v <= 0 || v > SOCKET_MAX_BUFFER)
            return -EINVAL;
        struct sockbuf* sb = optname == SOOPT_SNDBUF ? &so->snd : &so->rcv;
        sb->hiwat = (size_t)v;
        return 0;
    }
    default:
        return -ENOPROTOOPT;
    }
    if (v)
        so->options |= bit;
    else
        so->options &= ~bit;
    return 0;
}

static int sock_getopt_locked(struct socket* so, const int level, const int optname, void* optval, socklen_t* optlen)
{
    if (level != SOL_SOCKET) {
        if (!so->proto->pr_ctloutput)
            return -ENOPROTOOPT;
        return so->proto->pr_ctloutput(so, level, optname, optval, optlen, false);
    }

    if (optname == SOOPT_RCVTIMEO || optname == SOOPT_SNDTIMEO) {
        if (!optval || !optlen || *optlen < sizeof(struct sock_timeval))
            return -EINVAL;
        const uint64_t t = optname == SOOPT_RCVTIMEO ? so->rcv_timeo : so->snd_timeo;
        const uint64_t us = t * 1000000 / ticks_hz();
        const struct sock_timeval tv = { .tv_sec = (int64_t)(us / 1000000), .tv_usec = (int64_t)(us % 1000000) };
        memcpy(optval, &tv, sizeof(tv));
        *optlen = sizeof(tv);
        return 0;
    }

    if (!optval || !optlen || *optlen < sizeof(int))
        return -EINVAL;

    int v;
    switch (optname) {
    case SOOPT_REUSEADDR: v = !!(so->options & SO_REUSEADDR); break;
    case SOOPT_REUSEPORT: v = !!(so->options & SO_REUSEPORT); break;
    case SOOPT_BROADCAST: v = !!(so->options & SO_BROADCAST); break;
    case SOOPT_KEEPALIVE: v = !!(so->options & SO_KEEPALIVE); break;
    case SOOPT_TYPE: v = so->type; break;
    case SOOPT_ERROR: v = sock_take_error(so); break;
    case SOOPT_SNDBUF: v = (int)so->snd.hiwat; break;
    case SOOPT_RCVBUF: v = (int)so->rcv.hiwat; break;
    default:
        return -ENOPROTOOPT;
    }
    *(int*)optval = v;
    *optlen = sizeof(int);
    return 0;
}

static int sock_getsockname_locked(struct socket* so, struct sockaddr* addr, socklen_t* len)
{
    if (!addr || !len)
        return -EINVAL;
    if (!so->proto->pr_sockaddr)
        return -EOPNOTSUPP;
    return so->proto->pr_sockaddr(so, addr, len);
}

static int sock_getpeername_locked(struct socket* so, struct sockaddr* addr, socklen_t* len)
{
    if (!addr || !len)
        return -EINVAL;
    if (!(so->state & SS_ISCONNECTED))
        return -ENOTCONN;
    if (!so->proto->pr_peeraddr)
        return -EOPNOTSUPP;
    return so->proto->pr_peeraddr(so, addr, len);
}

void sock_is_connecting(struct socket* so)
{
    so->state &= ~(SS_ISCONNECTED | SS_ISDISCONNECTING);
    so->state |= SS_ISCONNECTING;
}

void sock_is_connected(struct socket* so)
{
    const bool was_connected = so->state & SS_ISCONNECTED;
    so->state &= ~(SS_ISCONNECTING | SS_ISDISCONNECTING);
    so->state |= SS_ISCONNECTED;

    if (!was_connected) {
        uint64_t flags;
        spinlock_acquire_irqsave(&accept_lock, &flags);
        struct socket* head = so->listen_head;
        if (head) {
            dlist_remove(&head->incomplete, &so->listen_link);
            dlist_insert_tail(&head->complete, &so->listen_link);
            so->embryonic = false;
            sock_hold(head);
        }
        spinlock_release_irqrestore(&accept_lock, flags);
        if (head) {
            sock_wakeup(head);
            sock_release(head);
        }
    }
    sock_wakeup(so);
}

void sock_is_disconnecting(struct socket* so)
{
    so->state &= ~SS_ISCONNECTING;
    so->state |= SS_ISDISCONNECTING | SS_CANTRCVMORE | SS_CANTSENDMORE;
    sock_wakeup(so);
}

void sock_is_disconnected(struct socket* so)
{
    so->state &= ~(SS_ISCONNECTING | SS_ISCONNECTED | SS_ISDISCONNECTING);
    so->state |= SS_CANTRCVMORE | SS_CANTSENDMORE;
    __atomic_or_fetch(&so->snd.flags, SB_CANTMORE, __ATOMIC_RELAXED);
    __atomic_or_fetch(&so->rcv.flags, SB_CANTMORE, __ATOMIC_RELAXED);
    sock_wakeup(so);
}

bool sock_drop_embryo(struct socket* so)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&accept_lock, &flags);
    struct socket* head = so->listen_head;
    if (!head || !so->embryonic) {
        spinlock_release_irqrestore(&accept_lock, flags);
        return false;
    }
    dlist_remove(&head->incomplete, &so->listen_link);
    so->listen_head = nullptr;
    so->embryonic = false;
    spinlock_release_irqrestore(&accept_lock, flags);
    sock_release(so);
    return true;
}

void sock_cant_send_more(struct socket* so)
{
    so->state |= SS_CANTSENDMORE;
    __atomic_or_fetch(&so->snd.flags, SB_CANTMORE, __ATOMIC_RELAXED);
    sock_wakeup(so);
}

void sock_cant_recv_more(struct socket* so)
{
    so->state |= SS_CANTRCVMORE;
    __atomic_or_fetch(&so->rcv.flags, SB_CANTMORE, __ATOMIC_RELAXED);
    sock_wakeup(so);
}

struct socket* sock_new_conn(struct socket* head)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&accept_lock, &flags);
    const bool full = head->incomplete.count + head->complete.count >= (size_t)head->backlog;
    spinlock_release_irqrestore(&accept_lock, flags);
    if (full)
        return nullptr;

    struct socket* so = sock_alloc(head->type, head->proto);
    if (!so)
        return nullptr;
    so->options = head->options & ~SO_ACCEPTCONN;
    so->linger = head->linger;
    so->snd.hiwat = head->snd.hiwat;
    so->rcv.hiwat = head->rcv.hiwat;

    so->listen_head = head;
    so->embryonic = true;
    spinlock_acquire_irqsave(&accept_lock, &flags);
    dlist_insert_tail(&head->incomplete, &so->listen_link);
    spinlock_release_irqrestore(&accept_lock, flags);
    return so;
}

void sock_hold(struct socket* so)
{
    __atomic_add_fetch(&so->refcount, 1, __ATOMIC_RELAXED);
}

void sock_release(struct socket* so)
{
    if (__atomic_load_n(&so->refcount, __ATOMIC_ACQUIRE) && __atomic_sub_fetch(&so->refcount, 1, __ATOMIC_ACQ_REL))
        return;
    sock_free(so);
}

uint64_t sock_poll_bit(const struct socket* so)
{
    return 1ULL << (((uintptr_t)so >> 6) * 0x9E3779B97F4A7C15ULL >> 58);
}

void sock_wakeup(struct socket* so)
{
    process_wakeup_poll(sock_poll_bit(so));
    sockbuf_wakeup(&so->rcv);
    sockbuf_wakeup(&so->snd);
    if (so->upcall)
        so->upcall(so, so->upcall_arg);
}

void sock_set_upcall(struct socket* so, void (*upcall)(struct socket* so, void* arg), void* arg)
{
    so->upcall = upcall;
    so->upcall_arg = arg;
}

#define SOCK_LOCKED(so, call)            \
    ({                                   \
        sock_lock(so);                   \
        const auto ret_ = (call);        \
        sock_unlock(so);                 \
        ret_;                            \
    })

int sock_bind(struct socket* so, const struct sockaddr* addr, const socklen_t len)
{
    return SOCK_LOCKED(so, sock_bind_locked(so, addr, len));
}

int sock_listen(struct socket* so, const int backlog)
{
    return SOCK_LOCKED(so, sock_listen_locked(so, backlog));
}

int sock_connect(struct socket* so, const struct sockaddr* addr, const socklen_t len)
{
    return SOCK_LOCKED(so, sock_connect_locked(so, addr, len));
}

int sock_accept(struct socket* so, struct socket** out, struct sockaddr* addr, socklen_t* len)
{
    return SOCK_LOCKED(so, sock_accept_locked(so, out, addr, len));
}

ssize_t sock_send(struct socket* so, const void* buf, const size_t len, const int flags,
                  const struct sockaddr* dst, const socklen_t dst_len)
{
    return SOCK_LOCKED(so, sock_send_locked(so, buf, len, flags, dst, dst_len));
}

ssize_t sock_send_prebuilt(struct socket* so, struct netbuf* nb, const void* buf, const size_t len)
{
    return SOCK_LOCKED(so, sock_send_prebuilt_locked(so, nb, buf, len));
}

ssize_t sock_recv(struct socket* so, void* buf, const size_t len, const int flags,
                  struct sockaddr* from, socklen_t* from_len)
{
    return SOCK_LOCKED(so, sock_recv_locked(so, buf, len, flags, from, from_len));
}

int sock_shutdown(struct socket* so, const int how)
{
    return SOCK_LOCKED(so, sock_shutdown_locked(so, how));
}

int sock_close(struct socket* so)
{
    sock_hold(so);
    const int err = SOCK_LOCKED(so, sock_close_locked(so));
    sock_release(so);
    return err;
}

int sock_setopt(struct socket* so, const int level, const int optname, const void* optval, const socklen_t optlen)
{
    return SOCK_LOCKED(so, sock_setopt_locked(so, level, optname, optval, optlen));
}

int sock_getopt(struct socket* so, const int level, const int optname, void* optval, socklen_t* optlen)
{
    return SOCK_LOCKED(so, sock_getopt_locked(so, level, optname, optval, optlen));
}

int sock_getsockname(struct socket* so, struct sockaddr* addr, socklen_t* len)
{
    return SOCK_LOCKED(so, sock_getsockname_locked(so, addr, len));
}

int sock_getpeername(struct socket* so, struct sockaddr* addr, socklen_t* len)
{
    return SOCK_LOCKED(so, sock_getpeername_locked(so, addr, len));
}
