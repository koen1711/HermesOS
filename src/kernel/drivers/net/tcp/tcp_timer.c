#include <drivers/net/tcp/tcp_timer.h>

#include <drivers/net/tcp/tcp_cc.h>
#include <drivers/net/tcp/tcp_output.h>
#include <drivers/net/tcp/tcp_sack.h>
#include <drivers/net/tcp/tcp_subr.h>
#include <drivers/net/inpcb/inpcb.h>
#include <drivers/net/socket/socket.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

static const callout_func_t tcp_timer_funcs[TCPT_COUNT] = {
    [TCPT_REXMT] = tcp_timer_rexmt,
    [TCPT_PERSIST] = tcp_timer_persist,
    [TCPT_KEEP] = tcp_timer_keep,
    [TCPT_2MSL] = tcp_timer_2msl,
    [TCPT_DELACK] = tcp_timer_delack,
};

uint64_t tcp_now_ms()
{
    return ticks_get() * 1000 / ticks_hz();
}

uint32_t tcp_rexmt_ms(const struct tcpcb* tp)
{
    const uint64_t rto = (uint64_t)tp->rto << tp->rexmt_shift;
    return rto > TCP_RTO_MAX_MS ? TCP_RTO_MAX_MS : (uint32_t)rto;
}

void tcp_timer_activate(struct tcpcb* tp, const enum tcp_timer_type which, const uint64_t delta_ms)
{
    callout_reset(&tp->timers[which], ticks_from_ms(delta_ms), tcp_timer_funcs[which], tp);
}

void tcp_timer_stop(struct tcpcb* tp, const enum tcp_timer_type which)
{
    callout_stop(&tp->timers[which]);
}

void tcp_timer_stop_all(struct tcpcb* tp)
{
    for (int i = 0; i < TCPT_COUNT; i++)
        callout_stop(&tp->timers[i]);
}

uint32_t tcp_timer_discard(struct tcpcb* tp)
{
    uint32_t pending = 0;
    for (int i = 0; i < TCPT_COUNT; i++) {
        callout_stop(&tp->timers[i]);
        pending += callout_dispatches(&tp->timers[i]) - tp->t_timer_entered[i];
    }
    return pending;
}

static bool tcp_timer_enter(struct tcpcb* tp, const enum tcp_timer_type which)
{
    tp->t_timer_entered[which]++;
    if (!tp->t_closed)
        return true;
    if (--tp->t_draining == 0)
        free(tp);
    return false;
}

bool tcp_timer_active(const struct tcpcb* tp, const enum tcp_timer_type which)
{
    return callout_pending(&tp->timers[which]);
}

static void tcp_rexmt_timeout(struct tcpcb* tp)
{
    if (++tp->rexmt_shift > TCP_MAXRXTSHIFT) {
        tcp_drop(tp, tp->t_softerror ? tp->t_softerror : ETIMEDOUT);
        return;
    }
    tcpstat.rexmttimeo++;

    if (tp->rexmt_shift > TCP_MAXRXTSHIFT / 4) {
        tp->rttvar += tp->srtt >> 3;
        tp->srtt = 0;
    }
    if (TCPS_HAVEESTABLISHED(tp->state))
        tp->cc->cong_signal(tp, CC_RTO);

    tp->snd_nxt = tp->snd_una;
    tp->snd_recover = tp->snd_max;
    tp->rtt_start = 0;
    tp->dupacks = 0;
    tp->t_flags &= ~TF_FASTRECOVERY;
    tcp_sack_clear(tp);
    tcp_timer_activate(tp, TCPT_REXMT, tcp_rexmt_ms(tp));
    tcp_output(tp);
}

void tcp_timer_rexmt(void* arg)
{
    uint64_t flags;
    tcp_lock(&flags);
    if (tcp_timer_enter(arg, TCPT_REXMT))
        tcp_rexmt_timeout(arg);
    tcp_unlock(flags);
}

void tcp_timer_persist(void* arg)
{
    struct tcpcb* tp = arg;
    uint64_t flags;
    tcp_lock(&flags);
    if (!tcp_timer_enter(tp, TCPT_PERSIST)) {
        tcp_unlock(flags);
        return;
    }
    tcpstat.persisttimeo++;
    tcp_setpersist(tp);
    tp->t_flags |= TF_FORCEDATA;
    tcp_output(tp);
    tp->t_flags &= ~TF_FORCEDATA;
    tcp_unlock(flags);
}

static bool tcp_keepalive_enabled(const struct tcpcb* tp)
{
    const struct socket* so = tp->inp->socket;
    return so && (so->options & SO_KEEPALIVE) && tp->state <= TCPS_CLOSE_WAIT;
}

static void tcp_keep_timeout(struct tcpcb* tp)
{
    if (!TCPS_HAVEESTABLISHED(tp->state)) {
        tcp_drop(tp, ETIMEDOUT);
        return;
    }
    if (!tcp_keepalive_enabled(tp)) {
        tcp_timer_activate(tp, TCPT_KEEP, TCP_KEEPIDLE_MS);
        return;
    }

    const uint64_t idle = tcp_now_ms() - tp->t_rcvtime;
    if (idle < TCP_KEEPIDLE_MS) {
        tp->keep_probes = 0;
        tcp_timer_activate(tp, TCPT_KEEP, TCP_KEEPIDLE_MS - idle);
        return;
    }
    if (tp->keep_probes >= TCP_KEEPCNT) {
        tcpstat.keeptimeo++;
        tcp_drop(tp, ETIMEDOUT);
        return;
    }
    tp->keep_probes++;
    tcp_respond(tp, nullptr, nullptr, tp->rcv_nxt, tp->snd_una - 1, TH_ACK);
    tcp_timer_activate(tp, TCPT_KEEP, TCP_KEEPINTVL_MS);
}

void tcp_timer_keep(void* arg)
{
    uint64_t flags;
    tcp_lock(&flags);
    if (tcp_timer_enter(arg, TCPT_KEEP))
        tcp_keep_timeout(arg);
    tcp_unlock(flags);
}

void tcp_timer_2msl(void* arg)
{
    uint64_t flags;
    tcp_lock(&flags);
    if (tcp_timer_enter(arg, TCPT_2MSL))
        tcp_close(arg);
    tcp_unlock(flags);
}

void tcp_timer_delack(void* arg)
{
    struct tcpcb* tp = arg;
    uint64_t flags;
    tcp_lock(&flags);
    if (!tcp_timer_enter(tp, TCPT_DELACK)) {
        tcp_unlock(flags);
        return;
    }
    tp->t_flags &= ~TF_DELACK;
    tp->t_flags |= TF_ACKNOW;
    tcp_output(tp);
    tcp_unlock(flags);
}
