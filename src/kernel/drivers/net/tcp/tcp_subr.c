#include <drivers/net/tcp/tcp_subr.h>

#include <drivers/net/tcp/tcp_cc.h>
#include <drivers/net/tcp/tcp_input.h>
#include <drivers/net/tcp/tcp_output.h>
#include <drivers/net/tcp/tcp_sack.h>
#include <drivers/net/tcp/tcp_timer.h>
#include <drivers/net/inpcb/inpcb.h>
#include <drivers/net/socket/socket.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/random.h>
#include <drivers/net/core/netif.h>
#include <drivers/net/core/route.h>
#include <drivers/net/core/siphash.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

struct inpcb_table tcp_pcbs;
struct tcp_stats tcpstat;

static uint8_t tcp_isn_key[SIPHASH_KEY_LEN];

void tcp_initialize()
{
    inpcb_table_init(&tcp_pcbs, "tcp");
    net_random_bytes(tcp_isn_key, sizeof(tcp_isn_key));
    tcp_cc_register(&tcp_cc_newreno);
    ipv4_register_protocol(IPPROTO_TCP, tcp_input4);
    ipv6_register_protocol(IPPROTO_TCP, tcp_input6);
}

void tcp_lock(uint64_t* flags)
{
    spinlock_acquire_irqsave(&tcp_pcbs.lock, flags);
}

void tcp_unlock(const uint64_t flags)
{
    spinlock_release_irqrestore(&tcp_pcbs.lock, flags);
}

static uint8_t tcp_pick_rcv_scale(const struct socket* so)
{
    const size_t hiwat = so ? so->rcv.hiwat : 0;
    uint8_t scale = 0;
    while (scale < TCP_MAX_WINSHIFT && (65535ul << scale) < hiwat)
        scale++;
    return scale;
}

struct tcpcb* tcp_newtcpcb(struct inpcb* inp)
{
    struct tcpcb* tp = calloc(1, sizeof(struct tcpcb));
    if (!tp)
        return nullptr;
    tp->inp = inp;
    tp->state = TCPS_CLOSED;
    for (int i = 0; i < TCPT_COUNT; i++)
        callout_init(&tp->timers[i]);
    tp->mss = (inp->flags & INPCB_IPV6) ? TCP_DEFAULT_MSS6 : TCP_DEFAULT_MSS;
    tp->rto = TCP_RTO_INITIAL_MS;
    tp->req_rcv_scale = tcp_pick_rcv_scale(inp->socket);
    tp->t_rcvtime = tcp_now_ms();
    tcp_cc_set(tp, tcp_cc_default());
    inp->ppcb = tp;
    return tp;
}

int tcp_attach_pcb(struct socket* so)
{
    struct inpcb* inp = inpcb_alloc(&tcp_pcbs, so);
    if (!inp)
        return -ENOMEM;
    if (!tcp_newtcpcb(inp)) {
        inpcb_free(inp);
        return -ENOMEM;
    }
    so->pcb = inp;
    so->refcount++;
    return 0;
}

void tcp_pcb_release(struct inpcb* inp)
{
    struct socket* so = inp->socket;
    inpcb_free(inp);
    if (so)
        sock_release(so);
}

struct tcpcb* tcp_close(struct tcpcb* tp)
{
    struct inpcb* inp = tp->inp;

    const uint32_t draining = tcp_timer_discard(tp);
    tcp_reassemble_flush(tp);
    if (tp->cc->destroy)
        tp->cc->destroy(tp);
    tcpstat.closed++;
    inp->ppcb = nullptr;
    if (draining) {
        tp->t_closed = true;
        tp->t_draining = draining;
    } else {
        free(tp);
    }

    if (inp->flags & INPCB_DETACHED) {
        tcp_pcb_release(inp);
    } else {
        inpcb_hash_remove(inp);
        sock_is_disconnected(inp->socket);
    }
    return nullptr;
}

struct tcpcb* tcp_drop(struct tcpcb* tp, const int error)
{
    struct socket* so = tp->inp->socket;
    if (TCPS_HAVERCVDSYN(tp->state)) {
        tcp_state_change(tp, TCPS_CLOSED);
        tcp_output(tp);
    }
    tcpstat.drops++;
    if (so && !(tp->inp->flags & INPCB_DETACHED))
        so->error = error;
    return tcp_close(tp);
}

tcp_seq tcp_new_isn(const struct inpcb* inp)
{
    const struct {
        struct in6_addr laddr;
        struct in6_addr faddr;
        uint16_t lport;
        uint16_t fport;
    } tuple = { inp->laddr, inp->faddr, inp->lport, inp->fport };
    const uint32_t h = (uint32_t)siphash24(tcp_isn_key, &tuple, sizeof(tuple));

    return (uint32_t)(tcp_now_ms() * 250) + h;
}

void tcp_sendseqinit(struct tcpcb* tp)
{
    tp->iss = tcp_new_isn(tp->inp);
    tp->snd_una = tp->snd_nxt = tp->snd_max = tp->snd_recover = tp->iss;
    tp->sack_rxmit = tp->iss;
}

void tcp_state_change(struct tcpcb* tp, const enum tcp_state new_state)
{
    tp->state = new_state;
}

const char* tcp_state_name(const enum tcp_state state)
{
    static const char* names[] = {
        "CLOSED", "LISTEN", "SYN_SENT", "SYN_RECEIVED", "ESTABLISHED", "CLOSE_WAIT",
        "FIN_WAIT_1", "CLOSING", "LAST_ACK", "FIN_WAIT_2", "TIME_WAIT",
    };
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
}

void tcp_established(struct tcpcb* tp)
{
    struct socket* so = tp->inp->socket;
    tcp_state_change(tp, TCPS_ESTABLISHED);
    tcpstat.connects++;
    if (tp->cc->conn_init)
        tp->cc->conn_init(tp);
    tcp_timer_activate(tp, TCPT_KEEP, TCP_KEEPIDLE_MS);
    if (so && !(tp->inp->flags & INPCB_DETACHED))
        sock_is_connected(so);
    if (tp->t_flags & TF_NEEDFIN) {
        tp->t_flags &= ~TF_NEEDFIN;
        tcp_state_change(tp, TCPS_FIN_WAIT_1);
    }
}

void tcp_enter_timewait(struct tcpcb* tp)
{
    tcp_state_change(tp, TCPS_TIME_WAIT);
    tcp_timer_stop_all(tp);
    tcp_timer_activate(tp, TCPT_2MSL, 2 * TCP_MSL_MS);
    if (!(tp->inp->flags & INPCB_DETACHED))
        sock_is_disconnected(tp->inp->socket);
}

struct tcpcb* tcp_usrclosed(struct tcpcb* tp)
{
    switch (tp->state) {
    case TCPS_CLOSED:
    case TCPS_LISTEN:
    case TCPS_SYN_SENT:
        return tcp_close(tp);
    case TCPS_SYN_RECEIVED:
        tp->t_flags |= TF_NEEDFIN;
        break;
    case TCPS_ESTABLISHED:
        tcp_state_change(tp, TCPS_FIN_WAIT_1);
        break;
    case TCPS_CLOSE_WAIT:
        tcp_state_change(tp, TCPS_LAST_ACK);
        break;
    default:
        break;
    }

    if (tp->state == TCPS_FIN_WAIT_2 && (tp->inp->flags & INPCB_DETACHED))
        tcp_timer_activate(tp, TCPT_2MSL, 2 * TCP_MSL_MS);
    return tp;
}

void tcp_packet_addrs(const struct netbuf* nb, struct in6_addr* src, struct in6_addr* dst)
{
    if ((nb->data[0] >> 4) == IPV4_VERSION) {
        const auto ip = (const struct ipv4_header*)nb->data;
        inpcb_map_v4(ip->src, src);
        inpcb_map_v4(ip->dst, dst);
    } else {
        const auto ip6 = (const struct ipv6_header*)nb->data;
        *src = ip6->src;
        *dst = ip6->dst;
    }
}

void tcp_ctlinput(const struct inpcb* inp, const int error, const uint32_t mtu)
{
    uint64_t flags;
    tcp_lock(&flags);
    struct tcpcb* tp = inp->ppcb;
    if (!tp) {
        tcp_unlock(flags);
        return;
    }

    if (mtu) {
        const uint32_t hdr = (inpcb_is_v4_mapped(&inp->faddr) ? IPV4_HEADER_LEN : IPV6_HEADER_LEN) + TCP_HEADER_LEN;
        if (mtu > hdr + 64 && mtu - hdr < tp->mss) {
            tp->mss = mtu - hdr;
            tp->snd_nxt = tp->snd_una;
            tcp_output(tp);
        }
    } else if (tp->state == TCPS_SYN_SENT && error == ECONNREFUSED) {
        tcp_drop(tp, error);
    } else {
        tp->t_softerror = error;
    }
    tcp_unlock(flags);
}

uint16_t tcp_local_mss(const struct tcpcb* tp)
{
    const struct in6_addr* dst = &tp->inp->faddr;
    const bool v4 = inpcb_is_v4_mapped(dst);
    struct route_nexthop nh;
    const bool found = v4 ? route_lookup4(inpcb_unmap_v4(dst), &nh) : route_lookup6(dst, &nh);
    const uint32_t mtu = found && nh.mtu ? nh.mtu : 1500;
    if (found)
        netif_rele(nh.nif);
    const uint32_t mss = mtu - (v4 ? IPV4_HEADER_LEN : IPV6_HEADER_LEN) - TCP_HEADER_LEN;
    return mss < TCP_MAX_PAYLOAD ? mss : TCP_MAX_PAYLOAD;
}
