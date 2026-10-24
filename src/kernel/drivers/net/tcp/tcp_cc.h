#ifndef NET_TCP_TCP_CC_H
#define NET_TCP_TCP_CC_H

#include <os/stdint.h>

struct tcpcb;

enum tcp_cc_signal {
    CC_DUPACK,
    CC_RTO,
    CC_ECN,
};

struct tcp_cc_algo {
    const char* name;
    int (*init)(struct tcpcb* tp);
    void (*destroy)(struct tcpcb* tp);
    void (*conn_init)(struct tcpcb* tp);
    void (*ack_received)(struct tcpcb* tp, uint32_t acked_bytes);
    void (*cong_signal)(struct tcpcb* tp, enum tcp_cc_signal signal);
    void (*post_recovery)(struct tcpcb* tp);
    void (*after_idle)(struct tcpcb* tp);
};

extern const struct tcp_cc_algo tcp_cc_newreno;

int tcp_cc_register(const struct tcp_cc_algo* algo);
const struct tcp_cc_algo* tcp_cc_find(const char* name);
const struct tcp_cc_algo* tcp_cc_default();
int tcp_cc_set(struct tcpcb* tp, const struct tcp_cc_algo* algo);

#endif //NET_TCP_TCP_CC_H
