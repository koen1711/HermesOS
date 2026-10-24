#ifndef NET_TCP_TCP_SACK_H
#define NET_TCP_TCP_SACK_H

#include <os/stdint.h>
#include <drivers/net/tcp/tcp_var.h>

void tcp_sack_update(struct tcpcb* tp, const struct tcp_sack_block* blocks, uint8_t n);

void tcp_sack_ack(struct tcpcb* tp, tcp_seq ack);
void tcp_sack_clear(struct tcpcb* tp);
uint32_t tcp_sack_bytes(const struct tcpcb* tp);

uint32_t tcp_sack_pipe(const struct tcpcb* tp);
bool tcp_sack_next_hole(const struct tcpcb* tp, tcp_seq* seq, uint32_t* len);
uint8_t tcp_sack_build_blocks(const struct tcpcb* tp, struct tcp_sack_block* out, uint8_t max);

#endif //NET_TCP_TCP_SACK_H
