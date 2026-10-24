#ifndef NET_TCP_TCP_H
#define NET_TCP_TCP_H

#include <os/stdint.h>

#define TCP_HEADER_LEN     20
#define TCP_MAX_HEADER_LEN 60
#define TCP_DEFAULT_MSS    536
#define TCP_DEFAULT_MSS6   1220
#define TCP_MAX_WINSHIFT   14
#define TCP_MAX_SACK       4

enum tcp_optname
{
    TCP_NODELAY = 1,
    TCP_MAXSEG = 2
};

enum tcp_flags
{
    TH_FIN = 0x01,
    TH_SYN = 0x02,
    TH_RST = 0x04,
    TH_PUSH = 0x08,
    TH_ACK = 0x10,
    TH_URG = 0x20,
    TH_ECE = 0x40,
    TH_CWR = 0x80
};


enum tcp_optkind
{
    TCPOPT_EOL = 0,
    TCPOPT_NOP = 1,
    TCPOPT_MAXSEG = 2,
    TCPOPT_WINDOW = 3,
    TCPOPT_SACK_PERMITTED = 4,
    TCPOPT_SACK = 5,
    TCPOPT_TIMESTAMP = 8
};

typedef uint32_t tcp_seq;

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((int32_t)((a) - (b)) >= 0)

struct tcp_sack_block {
    tcp_seq start;
    tcp_seq end;
};

struct tcp_header {
    uint16_t src_port;
    uint16_t dst_port;
    tcp_seq seq;
    tcp_seq ack;
    uint8_t data_off;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

#endif //NET_TCP_TCP_H
