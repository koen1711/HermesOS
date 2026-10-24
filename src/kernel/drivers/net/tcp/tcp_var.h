#ifndef NET_TCP_TCP_VAR_H
#define NET_TCP_TCP_VAR_H

#include <os/stdint.h>
#include <os/stdbool.h>
#include <drivers/timer/callout.h>
#include <drivers/net/tcp/tcp.h>
#include <drivers/net/core/netbuf.h>

enum tcp_state {
    TCPS_CLOSED,
    TCPS_LISTEN,
    TCPS_SYN_SENT,
    TCPS_SYN_RECEIVED,
    TCPS_ESTABLISHED,
    TCPS_CLOSE_WAIT,
    TCPS_FIN_WAIT_1,
    TCPS_CLOSING,
    TCPS_LAST_ACK,
    TCPS_FIN_WAIT_2,
    TCPS_TIME_WAIT,
};

enum tcp_timer_type {
    TCPT_REXMT,
    TCPT_PERSIST,
    TCPT_KEEP,
    TCPT_2MSL,
    TCPT_DELACK,
    TCPT_COUNT,
};

enum tcpcb_flags
{
    TF_ACKNOW = 0x0001,
    TF_DELACK = 0x0002,
    TF_NODELAY = 0x0004,
    TF_NOOPT = 0x0008,
    TF_SENTFIN = 0x0010,
    TF_RCVD_SCALE = 0x0020,
    TF_RCVD_TSTMP = 0x0040,
    TF_SACK_PERMIT = 0x0080,
    TF_FASTRECOVERY = 0x0100,
    TF_NEEDFIN = 0x0200,
    TF_FORCEDATA = 0x0400
};

#define TCP_SACK_SCOREBOARD 8
#define TCP_MAX_PAYLOAD (NETBUF_CLUSTER_SIZE - NETBUF_HEADROOM - TCP_MAX_HEADER_LEN)

#define TCPS_HAVERCVDSYN(s)   ((s) >= TCPS_SYN_RECEIVED)
#define TCPS_HAVEESTABLISHED(s) ((s) >= TCPS_ESTABLISHED)
#define TCPS_HAVERCVDFIN(s)   ((s) == TCPS_CLOSE_WAIT || (s) == TCPS_CLOSING || \
                               (s) == TCPS_LAST_ACK || (s) == TCPS_TIME_WAIT)

struct inpcb;
struct netbuf;
struct tcp_cc_algo;

struct tcp_reassemble_entry {
    struct tcp_reassemble_entry* next;
    tcp_seq seq;
    uint32_t len;
    uint8_t flags;
    struct netbuf* data;
};

struct tcpcb {
    struct inpcb* inp;
    enum tcp_state state;
    uint32_t t_flags;
    struct callout timers[TCPT_COUNT];
    uint32_t t_timer_entered[TCPT_COUNT];
    bool t_closed;
    uint32_t t_draining;
    uint8_t rexmt_shift;

    tcp_seq snd_una;
    tcp_seq snd_nxt;
    tcp_seq snd_max;
    tcp_seq snd_wl1;
    tcp_seq snd_wl2;
    tcp_seq iss;
    uint32_t snd_wnd;
    uint8_t snd_scale;

    tcp_seq rcv_nxt;
    tcp_seq irs;
    uint32_t rcv_wnd;
    tcp_seq rcv_adv;
    uint8_t rcv_scale;
    uint8_t req_rcv_scale;
    tcp_seq last_ack_sent;

    uint32_t snd_cwnd;
    uint32_t snd_ssthresh;
    tcp_seq snd_recover;
    uint32_t dupacks;
    const struct tcp_cc_algo* cc;
    void* cc_data;

    int32_t srtt;
    int32_t rttvar;
    uint32_t rto;
    uint64_t rtt_start;
    tcp_seq rtt_seq;

    uint32_t ts_recent;
    uint64_t ts_recent_age;

    uint16_t mss;
    uint16_t peer_mss;

    struct tcp_reassemble_entry* reassemble_queue;
    uint32_t reassemble_len;

    struct tcp_sack_block sack_blocks[TCP_SACK_SCOREBOARD];
    uint8_t sack_count;
    tcp_seq sack_rxmit;
    uint32_t sack_rexmit_bytes;

    uint64_t t_rcvtime;
    uint8_t keep_probes;
    int t_softerror;
};

struct tcp_stats {
    uint64_t connattempt, accepts, connects, drops, closed;
    uint64_t sndtotal, sndpack, sndbyte, sndrexmitpack, sndacks;
    uint64_t rcvtotal, rcvpack, rcvbyte, rcvbadsum, rcvduppack, rcvoopack;
    uint64_t rexmttimeo, persisttimeo, keeptimeo;
};

extern struct tcp_stats tcpstat;

#endif //NET_TCP_TCP_VAR_H
