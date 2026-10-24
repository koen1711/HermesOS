#include <drivers/net/tcp/tcp_input.h>

#include <drivers/net/tcp/tcp_cc.h>
#include <drivers/net/tcp/tcp_output.h>
#include <drivers/net/tcp/tcp_sack.h>
#include <drivers/net/tcp/tcp_subr.h>
#include <drivers/net/tcp/tcp_timer.h>
#include <drivers/net/inpcb/inpcb.h>
#include <drivers/net/socket/socket.h>
#include <drivers/net/core/byteorder.h>
#include <drivers/net/core/netbuf.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

#define TCP_PAWS_IDLE_MS (24ull * 24 * 60 * 60 * 1000)  // RFC 7323 5.5

struct tcp_seg {
    struct tcp_header* th;
    struct tcp_opts to;
    struct netbuf* data;
    uint32_t len;
    uint32_t wnd;
};

enum tcp_ack_result {
    TCP_ACK_CONTINUE,
    TCP_ACK_DROP,
    TCP_ACK_DROP_ACK,
    TCP_ACK_GONE,
};

static uint16_t tcp_get16(const uint8_t* p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t tcp_get32(const uint8_t* p)
{
    return (uint32_t)tcp_get16(p) << 16 | tcp_get16(p + 2);
}

static size_t tcp_header_len(const struct tcp_header* th)
{
    return (th->data_off >> 4) * 4;
}

static void tcp_seg_free(struct tcp_seg* seg)
{
    netbuf_free_chain(seg->data);
    seg->data = nullptr;
}

void tcp_dooptions(struct tcp_opts* to, const uint8_t* cp, size_t len, const bool is_syn)
{
    memset(to, 0, sizeof(struct tcp_opts));
    while (len > 0) {
        const uint8_t kind = cp[0];
        if (kind == TCPOPT_EOL)
            break;
        if (kind == TCPOPT_NOP) {
            cp++;
            len--;
            continue;
        }
        if (len < 2 || cp[1] < 2 || cp[1] > len)
            break;
        const uint8_t optlen = cp[1];

        if (kind == TCPOPT_MAXSEG && optlen == 4 && is_syn) {
            to->flags |= TOF_MSS;
            to->mss = tcp_get16(cp + 2);
        } else if (kind == TCPOPT_WINDOW && optlen == 3 && is_syn) {
            to->flags |= TOF_SCALE;
            to->wscale = cp[2] < TCP_MAX_WINSHIFT ? cp[2] : TCP_MAX_WINSHIFT;
        } else if (kind == TCPOPT_SACK_PERMITTED && optlen == 2 && is_syn) {
            to->flags |= TOF_SACKPERM;
        } else if (kind == TCPOPT_TIMESTAMP && optlen == 10) {
            to->flags |= TOF_TS;
            to->ts_val = tcp_get32(cp + 2);
            to->ts_ecr = tcp_get32(cp + 6);
        } else if (kind == TCPOPT_SACK && optlen >= 10 && (optlen - 2) % 8 == 0 && !is_syn) {
            to->flags |= TOF_SACK;
            to->nsacks = (optlen - 2) / 8 < TCP_MAX_SACK ? (optlen - 2) / 8 : TCP_MAX_SACK;
            for (uint8_t i = 0; i < to->nsacks; i++) {
                to->sacks[i].start = tcp_get32(cp + 2 + 8 * i);
                to->sacks[i].end = tcp_get32(cp + 6 + 8 * i);
            }
        }
        cp += optlen;
        len -= optlen;
    }
}

void tcp_xmit_timer(struct tcpcb* tp, int32_t rtt)
{
    if (rtt < 1)
        rtt = 1;
    if (!tp->srtt) {
        tp->srtt = rtt << 3;
        tp->rttvar = rtt << 1;
    } else {
        int32_t delta = rtt - (tp->srtt >> 3);
        tp->srtt += delta;
        if (delta < 0)
            delta = -delta;
        tp->rttvar += delta - (tp->rttvar >> 2);
    }

    const int32_t granularity = (int32_t)(1000 / ticks_hz()) + 1;
    uint32_t rto = (tp->srtt >> 3) + (tp->rttvar > granularity ? tp->rttvar : granularity);
    if (rto < TCP_RTO_MIN_MS)
        rto = TCP_RTO_MIN_MS;
    if (rto > TCP_RTO_MAX_MS)
        rto = TCP_RTO_MAX_MS;
    tp->rto = rto;
    tp->t_softerror = 0;
}

void tcp_mss_update(struct tcpcb* tp, const uint16_t offer)
{
    const bool v4 = inpcb_is_v4_mapped(&tp->inp->faddr);
    const uint32_t peer = offer ? offer : (v4 ? TCP_DEFAULT_MSS : TCP_DEFAULT_MSS6);
    uint32_t mss = tcp_local_mss(tp);
    if (peer < mss)
        mss = peer;
    if (mss < 64)
        mss = 64;
    tp->mss = mss;
    tp->peer_mss = offer;
}

static void tcp_reassemble_free_entry(struct tcpcb* tp, struct tcp_reassemble_entry* e)
{
    tp->reassemble_len -= e->len;
    netbuf_free_chain(e->data);
    free(e);
}

void tcp_reassemble_flush(struct tcpcb* tp)
{
    while (tp->reassemble_queue) {
        struct tcp_reassemble_entry* e = tp->reassemble_queue;
        tp->reassemble_queue = e->next;
        tcp_reassemble_free_entry(tp, e);
    }
}

static bool tcp_reassemble_full(const struct tcpcb* tp, const tcp_seq seq, const uint32_t len)
{
    const struct socket* so = tp->inp->socket;
    return seq != tp->rcv_nxt && so && tp->reassemble_len + len > so->rcv.hiwat;
}

static void tcp_reassemble_insert(struct tcpcb* tp, tcp_seq seq, uint32_t len, uint8_t flags, struct netbuf* nb)
{
    if (tcp_reassemble_full(tp, seq, len)) {
        netbuf_free_chain(nb);
        return;
    }

    struct tcp_reassemble_entry* prev = nullptr;
    struct tcp_reassemble_entry** pp = &tp->reassemble_queue;
    while (*pp && SEQ_LT((*pp)->seq, seq)) {
        prev = *pp;
        pp = &(*pp)->next;
    }

    if (prev) {
        const int32_t overlap = (int32_t)(prev->seq + prev->len - seq);
        if (overlap > 0 && (uint32_t)overlap >= len) {
            if ((uint32_t)overlap == len)
                prev->flags |= flags;
            netbuf_free_chain(nb);
            return;
        }
        if (overlap > 0) {
            netbuf_adj(nb, overlap);
            seq += overlap;
            len -= overlap;
        }
    }

    while (*pp && SEQ_LEQ((*pp)->seq + (*pp)->len, seq + len)) {
        struct tcp_reassemble_entry* covered = *pp;
        flags |= covered->flags;
        *pp = covered->next;
        tcp_reassemble_free_entry(tp, covered);
    }
    if (*pp && SEQ_LT((*pp)->seq, seq + len)) {
        const uint32_t overlap = seq + len - (*pp)->seq;
        netbuf_adj(nb, -(ssize_t)overlap);
        len -= overlap;
        flags &= ~TH_FIN;
    }

    struct tcp_reassemble_entry* e = malloc(sizeof(struct tcp_reassemble_entry));
    if (!e) {
        netbuf_free_chain(nb);
        return;
    }
    *e = (struct tcp_reassemble_entry){ .next = *pp, .seq = seq, .len = len, .flags = flags, .data = nb };
    *pp = e;
    tp->reassemble_len += len;
}

static int tcp_reassemble_pull(struct tcpcb* tp)
{
    struct socket* so = tp->inp->socket;
    int flags = 0;
    bool delivered = false;

    while (!(flags & TH_FIN) && tp->reassemble_queue && tp->reassemble_queue->seq == tp->rcv_nxt) {
        struct tcp_reassemble_entry* e = tp->reassemble_queue;
        tp->reassemble_queue = e->next;
        tp->rcv_nxt += e->len;
        tp->reassemble_len -= e->len;
        flags |= e->flags;
        tcpstat.rcvpack++;
        tcpstat.rcvbyte += e->len;
        if (e->len && so && !(so->state & SS_CANTRCVMORE)) {
            sockbuf_append(&so->rcv, e->data);
            delivered = true;
        } else {
            netbuf_free_chain(e->data);
        }
        free(e);
    }
    if (delivered)
        sock_wakeup(so);
    return flags & TH_FIN;
}

int tcp_reassemble(struct tcpcb* tp, struct tcp_header* th, struct netbuf* nb, const uint32_t len)
{
    if (th)
        tcp_reassemble_insert(tp, th->seq, len, th->flags & TH_FIN, nb);
    return tcp_reassemble_pull(tp);
}

static void tcp_apply_syn_opts(struct tcpcb* tp, const struct tcp_opts* to)
{
    if (!(tp->t_flags & TF_NOOPT)) {
        if (to->flags & TOF_SCALE) {
            tp->t_flags |= TF_RCVD_SCALE;
            tp->snd_scale = to->wscale;
            tp->rcv_scale = tp->req_rcv_scale;
        }
        if (to->flags & TOF_TS) {
            tp->t_flags |= TF_RCVD_TSTMP;
            tp->ts_recent = to->ts_val;
            tp->ts_recent_age = tcp_now_ms();
        }
        if (to->flags & TOF_SACKPERM)
            tp->t_flags |= TF_SACK_PERMIT;
    }
    tcp_mss_update(tp, (to->flags & TOF_MSS) ? to->mss : 0);
}

static void tcp_init_rcv(struct tcpcb* tp, const tcp_seq irs)
{
    tp->irs = irs;
    tp->rcv_nxt = irs + 1;
    tp->rcv_adv = tp->rcv_nxt;
}

static void tcp_reply_reset(struct netbuf* nb, struct tcp_header* th, uint32_t len)
{
    if (th->flags & TH_RST)
        return;
    if (th->flags & TH_ACK) {
        tcp_respond(nullptr, nb, th, 0, th->ack, TH_RST);
        return;
    }
    len += !!(th->flags & TH_SYN) + !!(th->flags & TH_FIN);
    tcp_respond(nullptr, nb, th, th->seq + len, 0, TH_RST | TH_ACK);
}

static void tcp_send_challenge_ack(struct tcpcb* tp)
{
    tcp_respond(tp, nullptr, nullptr, tp->rcv_nxt, tp->snd_nxt, TH_ACK);
}

static void tcp_input_listen(struct tcpcb* lp, struct netbuf* nb, struct tcp_seg* seg)
{
    struct tcp_header* th = seg->th;
    if (th->flags & TH_RST)
        return;
    if (th->flags & TH_ACK) {
        tcp_reply_reset(nb, th, seg->len);
        return;
    }
    if (!(th->flags & TH_SYN))
        return;

    struct socket* so = sock_new_conn(lp->inp->socket);
    if (!so)
        return;
    if (tcp_attach_pcb(so)) {
        sock_is_disconnected(so);
        return;
    }

    struct inpcb* inp = so->pcb;
    struct tcpcb* tp = inp->ppcb;
    tcp_packet_addrs(nb, &inp->faddr, &inp->laddr);
    inp->fport = th->src_port;
    inp->lport = lp->inp->lport;
    inp->flags |= lp->inp->flags & (INPCB_IPV6 | INPCB_IPV6_ONLY);
    inpcb_hash_insert(inp);

    tcp_apply_syn_opts(tp, &seg->to);
    tcp_init_rcv(tp, th->seq);
    tcp_sendseqinit(tp);
    tp->snd_wnd = seg->wnd;
    tp->snd_wl1 = th->seq;
    tcp_state_change(tp, TCPS_SYN_RECEIVED);
    tcp_timer_activate(tp, TCPT_KEEP, TCP_KEEPINIT_MS);
    tp->t_flags |= TF_ACKNOW;
    tcpstat.accepts++;
    tcp_output(tp);
}

static bool tcp_input_syn_sent(struct tcpcb* tp, struct tcp_seg* seg)
{
    struct tcp_header* th = seg->th;
    if ((th->flags & TH_ACK) && (SEQ_LEQ(th->ack, tp->iss) || SEQ_GT(th->ack, tp->snd_max))) {
        if (!(th->flags & TH_RST))
            tcp_respond(tp, nullptr, nullptr, 0, th->ack, TH_RST);
        return false;
    }
    if (th->flags & TH_RST) {
        if (th->flags & TH_ACK)
            tcp_drop(tp, ECONNREFUSED);
        return false;
    }
    if (!(th->flags & TH_SYN))
        return false;

    tcp_init_rcv(tp, th->seq);
    tcp_apply_syn_opts(tp, &seg->to);
    tp->t_flags |= TF_ACKNOW;

    if (!(th->flags & TH_ACK)) {
        tp->snd_wnd = seg->wnd;
        tp->snd_wl1 = th->seq;
        tp->snd_nxt = tp->iss;
        tcp_state_change(tp, TCPS_SYN_RECEIVED);
        tcp_output(tp);
        return false;
    }

    if (tp->rtt_start)
        tcp_xmit_timer(tp, (int32_t)(tcp_now_ms() - tp->rtt_start));
    tp->rtt_start = 0;
    tp->rexmt_shift = 0;
    tcp_timer_stop(tp, TCPT_REXMT);
    tp->snd_una = th->ack;
    if (SEQ_LT(tp->snd_nxt, tp->snd_una))
        tp->snd_nxt = tp->snd_una;
    tp->sack_rxmit = tp->snd_una;
    tcp_established(tp);

    tp->snd_wl1 = th->seq;
    th->seq++;
    th->flags &= ~TH_SYN;
    return true;
}

static uint32_t tcp_rcv_space(const struct tcpcb* tp)
{
    const struct socket* so = tp->inp->socket;
    const uint32_t space = so ? sockbuf_space(&so->rcv) : 0;
    const uint32_t advertised = SEQ_GT(tp->rcv_adv, tp->rcv_nxt) ? tp->rcv_adv - tp->rcv_nxt : 0;
    return space > advertised ? space : advertised;
}

static bool tcp_paws_ok(struct tcpcb* tp, const struct tcp_seg* seg)
{
    if (!(seg->to.flags & TOF_TS) || !(tp->t_flags & TF_RCVD_TSTMP) || (seg->th->flags & TH_RST))
        return true;
    if ((int32_t)(seg->to.ts_val - tp->ts_recent) >= 0)
        return true;
    if (tcp_now_ms() - tp->ts_recent_age > TCP_PAWS_IDLE_MS) {
        tp->ts_recent = 0;
        return true;
    }
    tcpstat.rcvduppack++;
    return false;
}

static void tcp_update_ts_recent(struct tcpcb* tp, const struct tcp_seg* seg)
{
    const struct tcp_header* th = seg->th;
    const uint32_t span = seg->len + !!(th->flags & (TH_SYN | TH_FIN));
    if ((seg->to.flags & TOF_TS) && SEQ_LEQ(th->seq, tp->last_ack_sent) &&
        SEQ_LEQ(tp->last_ack_sent, th->seq + span)) {
        tp->ts_recent = seg->to.ts_val;
        tp->ts_recent_age = tcp_now_ms();
    }
}

static void tcp_input_rst(struct tcpcb* tp, const struct tcp_seg* seg)
{
    const tcp_seq seq = seg->th->seq;
    if (seq != tp->rcv_nxt) {
        const uint32_t wnd = tp->rcv_wnd ? tp->rcv_wnd : 1;
        if (SEQ_GT(seq, tp->rcv_nxt) && SEQ_LT(seq, tp->rcv_nxt + wnd))
            tcp_send_challenge_ack(tp);
        return;
    }

    int error = 0;
    switch (tp->state) {
    case TCPS_SYN_RECEIVED:
        error = ECONNREFUSED;
        break;
    case TCPS_ESTABLISHED:
    case TCPS_FIN_WAIT_1:
    case TCPS_FIN_WAIT_2:
    case TCPS_CLOSE_WAIT:
        error = ECONNRESET;
        break;
    default:
        break;
    }
    struct socket* so = tp->inp->socket;
    if (error && so && !(tp->inp->flags & INPCB_DETACHED))
        so->error = error;
    tcp_close(tp);
}

static bool tcp_trim_segment(struct tcpcb* tp, struct tcp_seg* seg)
{
    struct tcp_header* th = seg->th;
    int32_t todrop = (int32_t)(tp->rcv_nxt - th->seq);
    if (todrop > 0) {
        if (th->flags & TH_SYN) {
            th->flags &= ~TH_SYN;
            th->seq++;
            todrop--;
        }
        if ((uint32_t)todrop > seg->len || ((uint32_t)todrop == seg->len && !(th->flags & TH_FIN))) {
            th->flags &= ~TH_FIN;
            tp->t_flags |= TF_ACKNOW;
            todrop = seg->len;
            tcpstat.rcvduppack++;
        }
        netbuf_adj(seg->data, todrop);
        th->seq += todrop;
        seg->len -= todrop;
    }

    todrop = (int32_t)(th->seq + seg->len - (tp->rcv_nxt + tp->rcv_wnd));
    if (todrop > 0) {
        if ((uint32_t)todrop >= seg->len) {
            if (tp->rcv_wnd || th->seq != tp->rcv_nxt)
                return false;
            tp->t_flags |= TF_ACKNOW;
            todrop = seg->len;
        }
        netbuf_adj(seg->data, -(ssize_t)todrop);
        seg->len -= todrop;
        th->flags &= ~(TH_PUSH | TH_FIN);
    }
    return true;
}

static void tcp_rtt_sample(struct tcpcb* tp, const struct tcp_seg* seg)
{
    const tcp_seq ack = seg->th->ack;
    if ((seg->to.flags & TOF_TS) && (tp->t_flags & TF_RCVD_TSTMP) && seg->to.ts_ecr)
        tcp_xmit_timer(tp, (int32_t)((uint32_t)tcp_now_ms() - seg->to.ts_ecr));
    else if (tp->rtt_start && SEQ_GT(ack, tp->rtt_seq))
        tcp_xmit_timer(tp, (int32_t)(tcp_now_ms() - tp->rtt_start));
    if (tp->rtt_start && SEQ_GT(ack, tp->rtt_seq))
        tp->rtt_start = 0;
}

static tcp_seq tcp_retransmit_from(struct tcpcb* tp, const tcp_seq from, const uint32_t cwnd)
{
    const tcp_seq onxt = tp->snd_nxt;
    tp->snd_nxt = from;
    tp->snd_cwnd = cwnd;
    tcp_output(tp);
    const tcp_seq end = tp->snd_nxt;
    if (SEQ_GT(onxt, tp->snd_nxt))
        tp->snd_nxt = onxt;
    return end;
}

static void tcp_enter_recovery(struct tcpcb* tp)
{
    tp->cc->cong_signal(tp, CC_DUPACK);
    tp->snd_recover = tp->snd_max;
    tcp_timer_stop(tp, TCPT_REXMT);
    tp->rtt_start = 0;

    const tcp_seq end = tcp_retransmit_from(tp, tp->snd_una, tp->mss);
    tp->t_flags |= TF_FASTRECOVERY;
    if (tp->t_flags & TF_SACK_PERMIT) {
        tp->sack_rxmit = end;
        tp->sack_rexmit_bytes = end - tp->snd_una;
        tp->snd_cwnd = tp->snd_ssthresh;
    } else {
        tp->snd_cwnd = tp->snd_ssthresh + tp->dupacks * tp->mss;
    }
}

static void tcp_dupack(struct tcpcb* tp, const struct tcp_seg* seg)
{
    const struct tcp_header* th = seg->th;
    const bool dup = !seg->len && seg->wnd == tp->snd_wnd && th->ack == tp->snd_una &&
                     tp->snd_max != tp->snd_una && !(th->flags & TH_FIN);
    if (!dup) {
        tp->dupacks = 0;
        return;
    }
    if (tp->t_flags & TF_FASTRECOVERY) {
        if (!(tp->t_flags & TF_SACK_PERMIT))
            tp->snd_cwnd += tp->mss;
        return;
    }
    if (++tp->dupacks == 3 && SEQ_GT(th->ack, tp->snd_recover))
        tcp_enter_recovery(tp);
}

static void tcp_partial_ack(struct tcpcb* tp, const tcp_seq ack)
{
    if (tp->t_flags & TF_SACK_PERMIT)
        return;

    const uint32_t acked = ack - tp->snd_una;
    const uint32_t ocwnd = tp->snd_cwnd;
    tcp_timer_stop(tp, TCPT_REXMT);
    tp->rtt_start = 0;
    tcp_retransmit_from(tp, ack, tp->mss + acked);
    tp->snd_cwnd = (ocwnd > acked ? ocwnd - acked : 0) + tp->mss;
}

static void tcp_exit_recovery(struct tcpcb* tp)
{
    tp->t_flags &= ~TF_FASTRECOVERY;
    tp->dupacks = 0;
    tp->sack_rexmit_bytes = 0;
    tp->cc->post_recovery(tp);
}

static void tcp_drop_acked_data(struct tcpcb* tp, const tcp_seq ack, uint32_t acked)
{
    if (SEQ_LEQ(tp->snd_una, tp->iss))
        acked--;
    if ((tp->t_flags & TF_SENTFIN) && ack == tp->snd_max)
        acked--;
    struct socket* so = tp->inp->socket;
    if (!so || !acked)
        return;
    sockbuf_drop(&so->snd, acked < so->snd.cc ? acked : so->snd.cc);
    sock_wakeup(so);
}

static void tcp_new_ack(struct tcpcb* tp, const struct tcp_seg* seg)
{
    const tcp_seq ack = seg->th->ack;
    const uint32_t acked = ack - tp->snd_una;

    tcp_rtt_sample(tp, seg);
    if (!(tp->t_flags & TF_FASTRECOVERY)) {
        tp->dupacks = 0;
        tp->cc->ack_received(tp, acked);
    } else if (SEQ_LT(ack, tp->snd_recover)) {
        tcp_partial_ack(tp, ack);
    } else {
        tcp_exit_recovery(tp);
    }

    tcp_drop_acked_data(tp, ack, acked);
    tp->snd_una = ack;
    if (SEQ_LT(tp->snd_nxt, ack))
        tp->snd_nxt = ack;
    tcp_sack_ack(tp, ack);
    tp->rexmt_shift = 0;

    if (ack == tp->snd_max)
        tcp_timer_stop(tp, TCPT_REXMT);
    else if (!tcp_timer_active(tp, TCPT_PERSIST))
        tcp_timer_activate(tp, TCPT_REXMT, tcp_rexmt_ms(tp));
}

static enum tcp_ack_result tcp_our_fin_acked(struct tcpcb* tp)
{
    if (!(tp->t_flags & TF_SENTFIN) || tp->snd_una != tp->snd_max)
        return TCP_ACK_CONTINUE;

    switch (tp->state) {
    case TCPS_FIN_WAIT_1:
        tcp_state_change(tp, TCPS_FIN_WAIT_2);
        if (tp->inp->flags & INPCB_DETACHED)
            tcp_timer_activate(tp, TCPT_2MSL, 2 * TCP_MSL_MS);
        break;
    case TCPS_CLOSING:
        tcp_enter_timewait(tp);
        break;
    case TCPS_LAST_ACK:
        tcp_close(tp);
        return TCP_ACK_GONE;
    default:
        break;
    }
    return TCP_ACK_CONTINUE;
}

static enum tcp_ack_result tcp_process_ack(struct tcpcb* tp, const struct tcp_seg* seg)
{
    const tcp_seq ack = seg->th->ack;
    if (tp->state == TCPS_SYN_RECEIVED) {
        if (SEQ_LEQ(ack, tp->snd_una) || SEQ_GT(ack, tp->snd_max)) {
            tcp_respond(tp, nullptr, nullptr, 0, ack, TH_RST);
            return TCP_ACK_DROP;
        }
        tcp_established(tp);
    }
    if (SEQ_GT(ack, tp->snd_max))
        return TCP_ACK_DROP_ACK;

    if ((tp->t_flags & TF_SACK_PERMIT) && (seg->to.flags & TOF_SACK))
        tcp_sack_update(tp, seg->to.sacks, seg->to.nsacks);

    if (SEQ_LEQ(ack, tp->snd_una)) {
        tcp_dupack(tp, seg);
        return TCP_ACK_CONTINUE;
    }
    tcp_new_ack(tp, seg);
    return tcp_our_fin_acked(tp);
}

static void tcp_update_window(struct tcpcb* tp, const struct tcp_seg* seg)
{
    const struct tcp_header* th = seg->th;
    if (SEQ_LT(tp->snd_wl1, th->seq) ||
        (tp->snd_wl1 == th->seq && (SEQ_LT(tp->snd_wl2, th->ack) ||
                                    (tp->snd_wl2 == th->ack && seg->wnd > tp->snd_wnd)))) {
        tp->snd_wnd = seg->wnd;
        tp->snd_wl1 = th->seq;
        tp->snd_wl2 = th->ack;
    }
}

static void tcp_process_fin(struct tcpcb* tp)
{
    struct socket* so = tp->inp->socket;
    if (so && !(tp->inp->flags & INPCB_DETACHED))
        sock_cant_recv_more(so);
    tp->rcv_nxt++;
    tp->t_flags |= TF_ACKNOW;

    switch (tp->state) {
    case TCPS_SYN_RECEIVED:
    case TCPS_ESTABLISHED:
        tcp_state_change(tp, TCPS_CLOSE_WAIT);
        break;
    case TCPS_FIN_WAIT_1:
        tcp_state_change(tp, TCPS_CLOSING);
        break;
    case TCPS_FIN_WAIT_2:
        tcp_enter_timewait(tp);
        break;
    default:
        break;
    }
}

static void tcp_schedule_ack(struct tcpcb* tp, const bool delay)
{
    if (!delay || (tp->t_flags & TF_DELACK)) {
        tp->t_flags |= TF_ACKNOW;
        return;
    }
    tp->t_flags |= TF_DELACK;
    tcp_timer_activate(tp, TCPT_DELACK, TCP_DELACK_MS);
}

static bool tcp_process_data(struct tcpcb* tp, struct tcp_seg* seg)
{
    struct tcp_header* th = seg->th;
    if (TCPS_HAVERCVDFIN(tp->state) || (!seg->len && !(th->flags & TH_FIN))) {
        tcp_seg_free(seg);
        return true;
    }
    if (seg->len && (tp->inp->flags & INPCB_DETACHED)) {
        tcp_seg_free(seg);
        tcp_drop(tp, 0);
        return false;
    }

    const bool in_order = th->seq == tp->rcv_nxt && !tp->reassemble_queue;
    if (!in_order)
        tcpstat.rcvoopack++;
    const int fin = tcp_reassemble(tp, th, seg->data, seg->len);
    seg->data = nullptr;
    if (seg->len)
        tcp_schedule_ack(tp, in_order);
    if (fin)
        tcp_process_fin(tp);
    return true;
}

static void tcp_input_sync(struct tcpcb* tp, struct tcp_seg* seg)
{
    const struct tcp_header* th = seg->th;
    tp->rcv_wnd = tcp_rcv_space(tp);

    if (!tcp_paws_ok(tp, seg))
        goto drop_after_ack;
    if (th->flags & TH_RST) {
        tcp_input_rst(tp, seg);
        goto drop;
    }
    if (!tcp_trim_segment(tp, seg))
        goto drop_after_ack;
    if (th->flags & TH_SYN) {
        tcp_send_challenge_ack(tp);
        goto drop;
    }
    if (!(th->flags & TH_ACK))
        goto drop;

    tcp_update_ts_recent(tp, seg);
    switch (tcp_process_ack(tp, seg)) {
    case TCP_ACK_CONTINUE:
        break;
    case TCP_ACK_DROP:
        goto drop;
    case TCP_ACK_DROP_ACK:
        goto drop_after_ack;
    case TCP_ACK_GONE:
        goto drop;
    }
    tcp_update_window(tp, seg);
    if (tcp_process_data(tp, seg))
        tcp_output(tp);
    return;

drop_after_ack:
    tcp_seg_free(seg);
    tp->t_flags |= TF_ACKNOW;
    tcp_output(tp);
    return;
drop:
    tcp_seg_free(seg);
}

void tcp_input(struct netbuf* nb, struct tcp_header* th, const size_t payload_offset, struct tcpcb* tp)
{
    struct tcp_seg seg = { .th = th, .len = nb->pkt_len - payload_offset };
    tcp_dooptions(&seg.to, (const uint8_t*)(th + 1), tcp_header_len(th) - TCP_HEADER_LEN, th->flags & TH_SYN);
    seg.wnd = (th->flags & TH_SYN) ? th->window : (uint32_t)th->window << tp->snd_scale;
    tp->t_rcvtime = tcp_now_ms();
    tp->keep_probes = 0;

    switch (tp->state) {
    case TCPS_CLOSED:
        tcp_reply_reset(nb, th, seg.len);
        netbuf_free_chain(nb);
        return;
    case TCPS_LISTEN:
        tcp_input_listen(tp, nb, &seg);
        netbuf_free_chain(nb);
        return;
    case TCPS_SYN_SENT:
        if (!tcp_input_syn_sent(tp, &seg)) {
            netbuf_free_chain(nb);
            return;
        }
        break;
    default:
        break;
    }

    netbuf_adj(nb, payload_offset);
    seg.data = nb;
    tcp_input_sync(tp, &seg);
}

static bool tcp_validate(const struct netbuf* nb, const size_t off, const size_t tlen,
                         const struct in6_addr* src, const struct in6_addr* dst)
{
    if (tlen < TCP_HEADER_LEN)
        return false;
    const auto th = (const struct tcp_header*)(nb->data + off);
    const size_t hlen = tcp_header_len(th);
    if (hlen < TCP_HEADER_LEN || hlen > tlen)
        return false;
    if (!(nb->csum_flags & NETBUF_CSUM_L4_VALID) && inpcb_l4_checksum(nb, off, tlen, IPPROTO_TCP, src, dst)) {
        tcpstat.rcvbadsum++;
        return false;
    }
    return true;
}

static void tcp_header_to_host(struct tcp_header* th)
{
    th->seq = ntohl(th->seq);
    th->ack = ntohl(th->ack);
    th->window = ntohs(th->window);
    th->urgent = ntohs(th->urgent);
}

static void tcp_input_packet(struct netbuf* nb, const size_t off, const struct in6_addr* src, const struct in6_addr* dst)
{
    tcpstat.rcvtotal++;
    const size_t tlen = nb->pkt_len - off;
    if (!tcp_validate(nb, off, tlen, src, dst)) {
        netbuf_free_chain(nb);
        return;
    }
    const size_t hlen = tcp_header_len((const struct tcp_header*)(nb->data + off));
    nb = netbuf_pullup(nb, off + hlen);
    if (!nb)
        return;
    const auto th = (struct tcp_header*)(nb->data + off);
    tcp_header_to_host(th);

    uint64_t flags;
    tcp_lock(&flags);
    struct inpcb* inp = inpcb_lookup(&tcp_pcbs, src, th->src_port, dst, th->dst_port, true);
    if (inp && inp->ppcb) {
        tcp_input(nb, th, off + hlen, inp->ppcb);
    } else {
        tcp_reply_reset(nb, th, tlen - hlen);
        netbuf_free_chain(nb);
    }
    tcp_unlock(flags);
}

void tcp_input4(struct netbuf* nb, const size_t header_len)
{
    nb = netbuf_pullup(nb, header_len + TCP_HEADER_LEN);
    if (!nb)
        return;
    const auto ip = (const struct ipv4_header*)nb->data;
    if ((nb->flags & (NETBUF_BCAST | NETBUF_MCAST)) || ipv4_is_multicast(ip->dst) ||
        ip->dst.s_addr == INADDR_BROADCAST) {
        netbuf_free_chain(nb);
        return;
    }
    struct in6_addr src, dst;
    inpcb_map_v4(ip->src, &src);
    inpcb_map_v4(ip->dst, &dst);
    tcp_input_packet(nb, header_len, &src, &dst);
}

void tcp_input6(struct netbuf* nb, const size_t offset)
{
    nb = netbuf_pullup(nb, offset + TCP_HEADER_LEN);
    if (!nb)
        return;
    const auto ip6 = (const struct ipv6_header*)nb->data;
    if ((nb->flags & NETBUF_MCAST) || ip6->dst.s6_addr[0] == 0xFF) {
        netbuf_free_chain(nb);
        return;
    }
    const struct in6_addr src = ip6->src;
    const struct in6_addr dst = ip6->dst;
    tcp_input_packet(nb, offset, &src, &dst);
}
