#include <drivers/net/tcp/tcp_cc.h>

#include <drivers/net/tcp/tcp_var.h>
#include <os/errno.h>

#define TCP_CC_MAX     4
#define TCP_CWND_MAX   (1u << 30)

static const struct tcp_cc_algo* tcp_cc_algos[TCP_CC_MAX];

static bool tcp_cc_name_eq(const char* a, const char* b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int tcp_cc_register(const struct tcp_cc_algo* algo)
{
    if (tcp_cc_find(algo->name))
        return -EEXIST;
    for (int i = 0; i < TCP_CC_MAX; i++) {
        if (!tcp_cc_algos[i]) {
            tcp_cc_algos[i] = algo;
            return 0;
        }
    }
    return -ENOMEM;
}

const struct tcp_cc_algo* tcp_cc_find(const char* name)
{
    for (int i = 0; i < TCP_CC_MAX; i++)
        if (tcp_cc_algos[i] && tcp_cc_name_eq(tcp_cc_algos[i]->name, name))
            return tcp_cc_algos[i];
    return nullptr;
}

const struct tcp_cc_algo* tcp_cc_default()
{
    return tcp_cc_algos[0] ? tcp_cc_algos[0] : &tcp_cc_newreno;
}

int tcp_cc_set(struct tcpcb* tp, const struct tcp_cc_algo* algo)
{
    if (tp->cc && tp->cc->destroy)
        tp->cc->destroy(tp);
    tp->cc = algo;
    tp->cc_data = nullptr;
    return algo->init ? algo->init(tp) : 0;
}

static uint32_t tcp_flight_size(const struct tcpcb* tp)
{
    return tp->snd_max - tp->snd_una;
}

static uint32_t newreno_initial_window(const struct tcpcb* tp)
{
    if (tp->mss > 2190)
        return 2 * tp->mss;
    if (tp->mss > 1095)
        return 3 * tp->mss;
    return 4 * tp->mss;
}

static uint32_t newreno_half_flight(const struct tcpcb* tp)
{
    const uint32_t half = tcp_flight_size(tp) / 2;
    return half > 2u * tp->mss ? half : 2u * tp->mss;
}

static int newreno_init(struct tcpcb* tp)
{
    tp->snd_cwnd = newreno_initial_window(tp);
    tp->snd_ssthresh = TCP_CWND_MAX;
    return 0;
}

static void newreno_conn_init(struct tcpcb* tp)
{
    tp->snd_cwnd = newreno_initial_window(tp);
}

static void newreno_ack_received(struct tcpcb* tp, const uint32_t acked_bytes)
{
    uint32_t incr;
    if (tp->snd_cwnd <= tp->snd_ssthresh)
        incr = acked_bytes < tp->mss ? acked_bytes : tp->mss;
    else
        incr = (uint32_t)tp->mss * tp->mss / tp->snd_cwnd;
    if (!incr)
        incr = 1;
    if (tp->snd_cwnd + incr <= TCP_CWND_MAX)
        tp->snd_cwnd += incr;
}

static void newreno_cong_signal(struct tcpcb* tp, const enum tcp_cc_signal signal)
{
    switch (signal) {
    case CC_DUPACK:
    case CC_ECN:
        tp->snd_ssthresh = newreno_half_flight(tp);
        tp->snd_cwnd = tp->snd_ssthresh;
        break;
    case CC_RTO:
        if (tp->rexmt_shift <= 1)
            tp->snd_ssthresh = newreno_half_flight(tp);
        tp->snd_cwnd = tp->mss;
        break;
    }
}

static void newreno_post_recovery(struct tcpcb* tp)
{
    tp->snd_cwnd = tp->snd_ssthresh;
}

static void newreno_after_idle(struct tcpcb* tp)
{
    const uint32_t iw = newreno_initial_window(tp);
    if (tp->snd_cwnd > iw)
        tp->snd_cwnd = iw;
}

const struct tcp_cc_algo tcp_cc_newreno = {
    .name = "newreno",
    .init = newreno_init,
    .conn_init = newreno_conn_init,
    .ack_received = newreno_ack_received,
    .cong_signal = newreno_cong_signal,
    .post_recovery = newreno_post_recovery,
    .after_idle = newreno_after_idle,
};
