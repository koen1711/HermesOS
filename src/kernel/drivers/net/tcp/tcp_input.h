#ifndef NET_TCP_TCP_INPUT_H
#define NET_TCP_TCP_INPUT_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <drivers/net/tcp/tcp_var.h>

struct netbuf;

enum tcp_opt_flags
{
    TOF_MSS = 0x01,
    TOF_SCALE = 0x02,
    TOF_SACKPERM = 0x04,
    TOF_TS = 0x08,
    TOF_SACK = 0x10
};

struct tcp_opts {
    uint32_t flags;
    uint16_t mss;
    uint8_t wscale;
    uint32_t ts_val;
    uint32_t ts_ecr;
    uint8_t nsacks;
    struct tcp_sack_block sacks[TCP_MAX_SACK];
};

void tcp_input4(struct netbuf* nb, size_t header_len);
void tcp_input6(struct netbuf* nb, size_t offset);

void tcp_input(struct netbuf* nb, struct tcp_header* th, size_t payload_offset, struct tcpcb* tp);

int tcp_reassemble(struct tcpcb* tp, struct tcp_header* th, struct netbuf* nb, uint32_t len);
void tcp_reassemble_flush(struct tcpcb* tp);
void tcp_dooptions(struct tcp_opts* to, const uint8_t* cp, size_t len, bool is_syn);

void tcp_xmit_timer(struct tcpcb* tp, int32_t rtt);
void tcp_mss_update(struct tcpcb* tp, uint16_t offer);

#endif //NET_TCP_TCP_INPUT_H
