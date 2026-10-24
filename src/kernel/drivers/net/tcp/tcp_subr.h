#ifndef NET_TCP_TCP_SUBR_H
#define NET_TCP_TCP_SUBR_H

#include <os/stdint.h>
#include <drivers/net/tcp/tcp_var.h>


struct inpcb;
struct inpcb_table;
struct socket;
struct netbuf;
struct in6_addr;

extern struct inpcb_table tcp_pcbs;

void tcp_initialize();

void tcp_lock(uint64_t* flags);
void tcp_unlock(uint64_t flags);

int tcp_attach_pcb(struct socket* so);
void tcp_pcb_release(struct inpcb* inp);
struct tcpcb* tcp_newtcpcb(struct inpcb* inp);

struct tcpcb* tcp_close(struct tcpcb* tp);
struct tcpcb* tcp_drop(struct tcpcb* tp, int error);

tcp_seq tcp_new_isn(const struct inpcb* inp);
void tcp_sendseqinit(struct tcpcb* tp);

uint16_t tcp_local_mss(const struct tcpcb* tp);
void tcp_state_change(struct tcpcb* tp, enum tcp_state new_state);
void tcp_established(struct tcpcb* tp);
void tcp_enter_timewait(struct tcpcb* tp);
struct tcpcb* tcp_usrclosed(struct tcpcb* tp);

void tcp_packet_addrs(const struct netbuf* nb, struct in6_addr* src, struct in6_addr* dst);
const char* tcp_state_name(enum tcp_state state);
void tcp_ctlinput(const struct inpcb* inp, int error, uint32_t mtu);

#endif //NET_TCP_TCP_SUBR_H
