#ifndef NET_TCP_TCP_TIMER_H
#define NET_TCP_TCP_TIMER_H

#include <os/stdint.h>
#include <drivers/net/tcp/tcp_var.h>


#define TCP_RTO_MIN_MS     1000
#define TCP_RTO_MAX_MS     60000
#define TCP_RTO_INITIAL_MS 1000
#define TCP_MAXRXTSHIFT    12
#define TCP_DELACK_MS      100
#define TCP_MSL_MS         30000
#define TCP_KEEPINIT_MS    75000
#define TCP_KEEPIDLE_MS    (2 * 60 * 60 * 1000)
#define TCP_KEEPINTVL_MS   75000
#define TCP_KEEPCNT        8
#define TCP_PERSMIN_MS     5000
#define TCP_PERSMAX_MS     60000

uint64_t tcp_now_ms();
uint32_t tcp_rexmt_ms(const struct tcpcb* tp);

void tcp_timer_activate(struct tcpcb* tp, enum tcp_timer_type which, uint64_t delta_ms);
void tcp_timer_stop(struct tcpcb* tp, enum tcp_timer_type which);
void tcp_timer_stop_all(struct tcpcb* tp);
bool tcp_timer_active(const struct tcpcb* tp, enum tcp_timer_type which);

void tcp_timer_rexmt(void* arg);
void tcp_timer_persist(void* arg);
void tcp_timer_keep(void* arg);
void tcp_timer_2msl(void* arg);
void tcp_timer_delack(void* arg);

#endif //NET_TCP_TCP_TIMER_H
