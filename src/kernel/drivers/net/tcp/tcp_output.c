#include <drivers/net/tcp/tcp_output.h>

#include <drivers/net/tcp/tcp_cc.h>
#include <drivers/net/tcp/tcp_input.h>
#include <drivers/net/tcp/tcp_sack.h>
#include <drivers/net/tcp/tcp_subr.h>
#include <drivers/net/tcp/tcp_timer.h>
#include <drivers/net/inpcb/inpcb.h>
#include <drivers/net/socket/socket.h>
#include <drivers/net/core/byteorder.h>
#include <drivers/net/core/netbuf.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

#define TCP_MAXWIN 65535u

static const uint8_t tcp_outflags[] = {
    [TCPS_CLOSED] = TH_RST | TH_ACK,
    [TCPS_LISTEN] = 0,
    [TCPS_SYN_SENT] = TH_SYN,
    [TCPS_SYN_RECEIVED] = TH_SYN | TH_ACK,
    [TCPS_ESTABLISHED] = TH_ACK,
    [TCPS_CLOSE_WAIT] = TH_ACK,
    [TCPS_FIN_WAIT_1] = TH_FIN | TH_ACK,
    [TCPS_CLOSING] = TH_FIN | TH_ACK,
    [TCPS_LAST_ACK] = TH_FIN | TH_ACK,
    [TCPS_FIN_WAIT_2] = TH_ACK,
    [TCPS_TIME_WAIT] = TH_ACK,
};

struct tcp_txplan {
    tcp_seq seq;
    uint32_t off;
    uint32_t len;
    uint8_t flags;
    bool sack_rxmit;
};

static uint8_t* tcp_put16(uint8_t* p, const uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v;
    return p + 2;
}

static uint8_t* tcp_put32(uint8_t* p, const uint32_t v)
{
    return tcp_put16(tcp_put16(p, v >> 16), v);
}

static uint8_t* tcp_put_opt_header(uint8_t* p, const uint8_t kind, const uint8_t len)
{
    *p++ = TCPOPT_NOP;
    *p++ = TCPOPT_NOP;
    *p++ = kind;
    *p++ = len;
    return p;
}

size_t tcp_addoptions(struct tcp_opts* to, uint8_t* opt)
{
    uint8_t* p = opt;
    if (to->flags & TOF_MSS) {
        *p++ = TCPOPT_MAXSEG;
        *p++ = 4;
        p = tcp_put16(p, to->mss);
    }
    if (to->flags & TOF_SCALE) {
        *p++ = TCPOPT_NOP;
        *p++ = TCPOPT_WINDOW;
        *p++ = 3;
        *p++ = to->wscale;
    }
    if (to->flags & TOF_SACKPERM)
        p = tcp_put_opt_header(p, TCPOPT_SACK_PERMITTED, 2);
    if (to->flags & TOF_TS) {
        p = tcp_put_opt_header(p, TCPOPT_TIMESTAMP, 10);
        p = tcp_put32(p, to->ts_val);
        p = tcp_put32(p, to->ts_ecr);
    }
    if (to->flags & TOF_SACK) {
        const size_t room = (TCP_MAX_HEADER_LEN - TCP_HEADER_LEN - (p - opt) - 4) / 8;
        const uint8_t n = to->nsacks < room ? to->nsacks : room;
        if (n) {
            p = tcp_put_opt_header(p, TCPOPT_SACK, 2 + 8 * n);
            for (uint8_t i = 0; i < n; i++) {
                p = tcp_put32(p, to->sacks[i].start);
                p = tcp_put32(p, to->sacks[i].end);
            }
        }
    }
    return p - opt;
}

static size_t tcp_build_opts(const struct tcpcb* tp, const uint8_t flags, uint8_t* opt)
{
    struct tcp_opts to = {};
    const bool offer = !(tp->t_flags & TF_NOOPT);
    const bool active = tp->state == TCPS_SYN_SENT;

    if (flags & TH_SYN) {
        to.flags |= TOF_MSS;
        to.mss = tcp_local_mss(tp);
        if (offer && (active || (tp->t_flags & TF_RCVD_SCALE))) {
            to.flags |= TOF_SCALE;
            to.wscale = tp->req_rcv_scale;
        }
        if (offer && (active || (tp->t_flags & TF_SACK_PERMIT)))
            to.flags |= TOF_SACKPERM;
        if (offer && (active || (tp->t_flags & TF_RCVD_TSTMP)))
            to.flags |= TOF_TS;
    } else {
        if (tp->t_flags & TF_RCVD_TSTMP)
            to.flags |= TOF_TS;
        if ((tp->t_flags & TF_SACK_PERMIT) && tp->reassemble_queue) {
            to.nsacks = tcp_sack_build_blocks(tp, to.sacks, TCP_MAX_SACK);
            if (to.nsacks)
                to.flags |= TOF_SACK;
        }
    }
    to.ts_val = (uint32_t)tcp_now_ms();
    to.ts_ecr = tp->ts_recent;
    return tcp_addoptions(&to, opt);
}

static uint32_t tcp_rcv_window(const struct tcpcb* tp)
{
    const struct socket* so = tp->inp->socket;
    uint32_t win = so ? sockbuf_space(&so->rcv) : 0;

    if (so && win < so->rcv.hiwat / 4 && win < tp->mss)
        win = 0;
    const uint32_t max = TCP_MAXWIN << tp->rcv_scale;
    if (win > max)
        win = max;

    const uint32_t advertised = SEQ_GT(tp->rcv_adv, tp->rcv_nxt) ? tp->rcv_adv - tp->rcv_nxt : 0;
    return win > advertised ? win : advertised;
}

static uint16_t tcp_window_field(const struct tcpcb* tp, const uint32_t win, const uint8_t flags)
{
    const uint32_t w = win >> ((flags & TH_SYN) ? 0 : tp->rcv_scale);
    return w > TCP_MAXWIN ? TCP_MAXWIN : w;
}

static bool tcp_in_sack_recovery(const struct tcpcb* tp)
{
    return (tp->t_flags & TF_FASTRECOVERY) && (tp->t_flags & TF_SACK_PERMIT);
}

static uint32_t tcp_send_window(const struct tcpcb* tp)
{
    uint32_t cwnd = tp->snd_cwnd;
    if (tcp_in_sack_recovery(tp)) {
        const uint32_t pipe = tcp_sack_pipe(tp);
        cwnd = (tp->snd_nxt - tp->snd_una) + (cwnd > pipe ? cwnd - pipe : 0);
    }
    const uint32_t win = tp->snd_wnd < cwnd ? tp->snd_wnd : cwnd;
    return !win && (tp->t_flags & TF_FORCEDATA) ? 1 : win;
}

static uint32_t tcp_max_payload(const struct tcpcb* tp, const size_t optlen)
{
    const uint32_t max = tp->mss > optlen ? tp->mss - optlen : 1;
    return max < TCP_MAX_PAYLOAD ? max : TCP_MAX_PAYLOAD;
}

static bool tcp_plan_sack_rxmit(const struct tcpcb* tp, const uint32_t maxseg, struct tcp_txplan* tx)
{
    if (!tcp_in_sack_recovery(tp))
        return false;
    tcp_seq seq;
    uint32_t len;
    if (!tcp_sack_next_hole(tp, &seq, &len))
        return false;

    const uint32_t pipe = tcp_sack_pipe(tp);
    if (tp->snd_cwnd <= pipe)
        return true;
    const uint32_t room = tp->snd_cwnd - pipe;
    if (len > maxseg)
        len = maxseg;
    tx->seq = seq;
    tx->off = seq - tp->snd_una;
    tx->len = len < room ? len : room;
    tx->sack_rxmit = true;
    return true;
}

static void tcp_plan_data(const struct tcpcb* tp, const uint32_t sb_cc, const uint32_t maxseg, struct tcp_txplan* tx)
{
    if (!TCPS_HAVEESTABLISHED(tp->state))
        return;
    const uint32_t win = tcp_send_window(tp);
    const uint32_t end = sb_cc < win ? sb_cc : win;
    tx->off = tp->snd_nxt - tp->snd_una;
    tx->len = end > tx->off ? end - tx->off : 0;
    if (tx->len > maxseg)
        tx->len = maxseg;
}

static void tcp_plan_fin(const struct tcpcb* tp, const uint32_t sb_cc, struct tcp_txplan* tx)
{
    if (!(tx->flags & TH_FIN))
        return;
    const bool more_data = tx->sack_rxmit || tx->off + tx->len < sb_cc;
    const bool already_sent = (tp->t_flags & TF_SENTFIN) && tp->snd_nxt == tp->snd_max;
    if (more_data || already_sent)
        tx->flags &= ~TH_FIN;
}

static bool tcp_nagle_allows(const struct tcpcb* tp, const uint32_t sb_cc, const struct tcp_txplan* tx)
{
    const bool idle = tp->snd_max == tp->snd_una;
    return (idle || (tp->t_flags & TF_NODELAY)) && tx->off + tx->len >= sb_cc;
}

static bool tcp_window_update_due(const struct tcpcb* tp)
{
    if (!TCPS_HAVEESTABLISHED(tp->state))
        return false;
    const struct socket* so = tp->inp->socket;
    const uint32_t win = tcp_rcv_window(tp);
    const uint32_t advertised = SEQ_GT(tp->rcv_adv, tp->rcv_nxt) ? tp->rcv_adv - tp->rcv_nxt : 0;
    if (win <= advertised)
        return false;
    const uint32_t incr = win - advertised;
    return incr >= 2u * tp->mss || (so && 2 * incr >= so->rcv.hiwat);
}

static bool tcp_should_send(const struct tcpcb* tp, const uint32_t sb_cc, const uint32_t maxseg,
                            const struct tcp_txplan* tx)
{
    if (tx->len) {
        if (tx->len >= maxseg || tx->sack_rxmit || (tp->t_flags & TF_FORCEDATA))
            return true;
        if (SEQ_LT(tp->snd_nxt, tp->snd_max) || tcp_nagle_allows(tp, sb_cc, tx))
            return true;
    }
    if (tp->t_flags & TF_ACKNOW)
        return true;
    if (tx->flags & (TH_SYN | TH_RST | TH_FIN))
        return true;
    return tcp_window_update_due(tp);
}

static void tcp_maybe_persist(struct tcpcb* tp, const uint32_t sb_cc)
{
    if (!sb_cc || !TCPS_HAVEESTABLISHED(tp->state))
        return;
    if (tcp_timer_active(tp, TCPT_REXMT) || tcp_timer_active(tp, TCPT_PERSIST))
        return;
    tp->rexmt_shift = 0;
    tcp_setpersist(tp);
}

static void tcp_fill_header(struct tcp_header* th, const uint16_t sport, const uint16_t dport,
                            const tcp_seq seq, const tcp_seq ack, const uint8_t flags,
                            const uint16_t win, const size_t hlen)
{
    th->src_port = sport;
    th->dst_port = dport;
    th->seq = htonl(seq);
    th->ack = (flags & TH_ACK) ? htonl(ack) : 0;
    th->data_off = (hlen / 4) << 4;
    th->flags = flags;
    th->window = htons(win);
    th->checksum = 0;
    th->urgent = 0;
}

static void tcp_checksum_and_send(const struct inpcb* inp, struct netbuf* nb,
                                  const struct in6_addr* src, const struct in6_addr* dst)
{
    const auto th = (struct tcp_header*)nb->data;
    th->checksum = htons(inpcb_l4_checksum(nb, 0, nb->len, IPPROTO_TCP, src, dst));
    tcpstat.sndtotal++;
    inpcb_ip_output(inp, nb, IPPROTO_TCP, src, dst);
}

static struct netbuf* tcp_build_segment(const struct tcpcb* tp, const struct tcp_txplan* tx,
                                        const uint8_t* opt, const size_t optlen, const uint32_t win)
{
    struct netbuf* nb = netbuf_alloc_cluster(true);
    if (!nb)
        return nullptr;

    const struct inpcb* inp = tp->inp;
    const size_t hlen = TCP_HEADER_LEN + optlen;
    const auto th = (struct tcp_header*)nb->data;
    tcp_fill_header(th, inp->lport, inp->fport, tx->seq, tp->rcv_nxt, tx->flags,
                    tcp_window_field(tp, win, tx->flags), hlen);
    memcpy(th + 1, opt, optlen);
    if (tx->len)
        sockbuf_copy(&inp->socket->snd, tx->off, tx->len, nb->data + hlen);
    nb->len = nb->pkt_len = hlen + tx->len;
    return nb;
}

static void tcp_count_send(const struct tcpcb* tp, const struct tcp_txplan* tx)
{
    if (tx->len && SEQ_LT(tx->seq, tp->snd_max)) {
        tcpstat.sndrexmitpack++;
    } else if (tx->len) {
        tcpstat.sndpack++;
        tcpstat.sndbyte += tx->len;
    } else if (tx->flags & TH_ACK) {
        tcpstat.sndacks++;
    }
}

static void tcp_after_send(struct tcpcb* tp, const struct tcp_txplan* tx, const uint32_t win)
{
    if (win && SEQ_GT(tp->rcv_nxt + win, tp->rcv_adv))
        tp->rcv_adv = tp->rcv_nxt + win;
    tp->last_ack_sent = tp->rcv_nxt;
    tp->t_flags &= ~(TF_ACKNOW | TF_DELACK);
    tcp_timer_stop(tp, TCPT_DELACK);
    if (tx->flags & TH_RST)
        return;

    const tcp_seq end = tx->seq + tx->len + !!(tx->flags & TH_SYN) + !!(tx->flags & TH_FIN);
    if (tx->flags & TH_FIN)
        tp->t_flags |= TF_SENTFIN;
    if (tx->sack_rxmit) {
        tp->sack_rxmit = end;
        tp->sack_rexmit_bytes += tx->len;
    }

    const bool probe = (tp->t_flags & TF_FORCEDATA) && tcp_timer_active(tp, TCPT_PERSIST);
    if (!probe && !tx->sack_rxmit)
        tp->snd_nxt = end;
    if (SEQ_GT(end, tp->snd_max)) {
        tp->snd_max = end;
        if (!tp->rtt_start && !probe) {
            const uint64_t now = tcp_now_ms();
            tp->rtt_start = now ? now : 1;
            tp->rtt_seq = tx->seq;
        }
    }

    if (!probe && tp->snd_nxt != tp->snd_una && !tcp_timer_active(tp, TCPT_REXMT)) {
        if (tcp_timer_active(tp, TCPT_PERSIST)) {
            tcp_timer_stop(tp, TCPT_PERSIST);
            tp->rexmt_shift = 0;
        }
        tcp_timer_activate(tp, TCPT_REXMT, tcp_rexmt_ms(tp));
    }
}

static int tcp_output_once(struct tcpcb* tp)
{
    const struct socket* so = tp->inp->socket;
    const uint32_t sb_cc = so ? so->snd.cc : 0;
    struct tcp_txplan tx = { .seq = tp->snd_nxt, .flags = tcp_outflags[tp->state] };

    if ((tx.flags & TH_SYN) && tp->snd_nxt != tp->iss)
        tx.flags &= ~TH_SYN;

    uint8_t opt[TCP_MAX_HEADER_LEN - TCP_HEADER_LEN];
    const size_t optlen = tcp_build_opts(tp, tx.flags, opt);
    const uint32_t maxseg = tcp_max_payload(tp, optlen);

    if (!tcp_plan_sack_rxmit(tp, maxseg, &tx))
        tcp_plan_data(tp, sb_cc, maxseg, &tx);
    tcp_plan_fin(tp, sb_cc, &tx);

    if (!tcp_should_send(tp, sb_cc, maxseg, &tx)) {
        tcp_maybe_persist(tp, sb_cc);
        return 0;
    }

    const uint32_t win = tcp_rcv_window(tp);
    struct netbuf* nb = tcp_build_segment(tp, &tx, opt, optlen, win);
    if (!nb)
        return -ENOBUFS;
    tcp_count_send(tp, &tx);
    tcp_after_send(tp, &tx, win);
    tcp_checksum_and_send(tp->inp, nb, &tp->inp->laddr, &tp->inp->faddr);

    const bool full = tx.len == maxseg || tx.sack_rxmit;
    return full && !(tx.flags & TH_RST) && !(tp->t_flags & TF_FORCEDATA);
}

int tcp_output(struct tcpcb* tp)
{
    const bool idle = tp->snd_max == tp->snd_una;
    if (idle && TCPS_HAVEESTABLISHED(tp->state) && tp->cc->after_idle &&
        tcp_now_ms() - tp->t_rcvtime >= tp->rto)
        tp->cc->after_idle(tp);

    int ret;
    while ((ret = tcp_output_once(tp)) > 0)
        ;
    return ret;
}

void tcp_respond(const struct tcpcb* tp, const struct netbuf* orig, const struct tcp_header* th,
                 const tcp_seq ack, const tcp_seq seq, const uint8_t flags)
{
    struct in6_addr src, dst;
    uint16_t sport, dport;
    if (tp) {
        src = tp->inp->laddr;
        dst = tp->inp->faddr;
        sport = tp->inp->lport;
        dport = tp->inp->fport;
    } else {
        tcp_packet_addrs(orig, &dst, &src);
        sport = th->dst_port;
        dport = th->src_port;
    }

    struct netbuf* nb = netbuf_alloc(true);
    if (!nb)
        return;
    const uint16_t win = tp ? tcp_window_field(tp, tcp_rcv_window(tp), 0) : 0;
    tcp_fill_header((struct tcp_header*)nb->data, sport, dport, seq, ack, flags, win, TCP_HEADER_LEN);
    nb->len = nb->pkt_len = TCP_HEADER_LEN;
    tcp_checksum_and_send(tp ? tp->inp : nullptr, nb, &src, &dst);
}

void tcp_setpersist(struct tcpcb* tp)
{
    uint64_t t = (uint64_t)tp->rto << tp->rexmt_shift;
    if (t < TCP_PERSMIN_MS)
        t = TCP_PERSMIN_MS;
    if (t > TCP_PERSMAX_MS)
        t = TCP_PERSMAX_MS;
    tcp_timer_activate(tp, TCPT_PERSIST, t);
    if (tp->rexmt_shift < TCP_MAXRXTSHIFT)
        tp->rexmt_shift++;
}
