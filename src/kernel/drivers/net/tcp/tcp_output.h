#ifndef NET_TCP_TCP_OUTPUT_H
#define NET_TCP_TCP_OUTPUT_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <drivers/net/tcp/tcp_var.h>

struct netbuf;
struct tcp_opts;

int tcp_output(struct tcpcb* tp);
void tcp_respond(const struct tcpcb* tp, const struct netbuf* orig, const struct tcp_header* th,
                 tcp_seq ack, tcp_seq seq, uint8_t flags);
void tcp_setpersist(struct tcpcb* tp);

size_t tcp_addoptions(struct tcp_opts* to, uint8_t* opt);

#endif //NET_TCP_TCP_OUTPUT_H
