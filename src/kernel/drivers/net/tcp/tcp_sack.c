#include <drivers/net/tcp/tcp_sack.h>

static void tcp_sack_remove(struct tcpcb* tp, const uint8_t i)
{
    for (uint8_t j = i; j + 1 < tp->sack_count; j++)
        tp->sack_blocks[j] = tp->sack_blocks[j + 1];
    tp->sack_count--;
}

static void tcp_sack_insert_at(struct tcpcb* tp, const uint8_t i, const struct tcp_sack_block b)
{
    if (tp->sack_count == TCP_SACK_SCOREBOARD) {
        if (i == TCP_SACK_SCOREBOARD)
            return;
        tp->sack_count--;
    }
    for (uint8_t j = tp->sack_count; j > i; j--)
        tp->sack_blocks[j] = tp->sack_blocks[j - 1];
    tp->sack_blocks[i] = b;
    tp->sack_count++;
}

static void tcp_sack_merge(struct tcpcb* tp, struct tcp_sack_block b)
{
    uint8_t i = 0;
    while (i < tp->sack_count) {
        const struct tcp_sack_block* cur = &tp->sack_blocks[i];
        if (SEQ_LT(cur->end, b.start)) {
            i++;
            continue;
        }
        if (SEQ_GT(cur->start, b.end))
            break;
        if (SEQ_LT(cur->start, b.start))
            b.start = cur->start;
        if (SEQ_GT(cur->end, b.end))
            b.end = cur->end;
        tcp_sack_remove(tp, i);
    }
    tcp_sack_insert_at(tp, i, b);
}

void tcp_sack_update(struct tcpcb* tp, const struct tcp_sack_block* blocks, const uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        struct tcp_sack_block b = blocks[i];
        if (SEQ_LEQ(b.end, b.start) || SEQ_LEQ(b.end, tp->snd_una) || SEQ_GT(b.end, tp->snd_max))
            continue;
        if (SEQ_LT(b.start, tp->snd_una))
            b.start = tp->snd_una;
        tcp_sack_merge(tp, b);
    }
}

void tcp_sack_ack(struct tcpcb* tp, const tcp_seq ack)
{
    while (tp->sack_count && SEQ_LEQ(tp->sack_blocks[0].end, ack))
        tcp_sack_remove(tp, 0);
    if (tp->sack_count && SEQ_LT(tp->sack_blocks[0].start, ack))
        tp->sack_blocks[0].start = ack;
    if (SEQ_LT(tp->sack_rxmit, ack))
        tp->sack_rxmit = ack;
}

void tcp_sack_clear(struct tcpcb* tp)
{
    tp->sack_count = 0;
    tp->sack_rxmit = tp->snd_una;
    tp->sack_rexmit_bytes = 0;
}

uint32_t tcp_sack_bytes(const struct tcpcb* tp)
{
    uint32_t bytes = 0;
    for (uint8_t i = 0; i < tp->sack_count; i++)
        bytes += tp->sack_blocks[i].end - tp->sack_blocks[i].start;
    return bytes;
}

uint32_t tcp_sack_pipe(const struct tcpcb* tp)
{
    return tp->snd_max - tp->snd_una - tcp_sack_bytes(tp) + tp->sack_rexmit_bytes;
}

bool tcp_sack_next_hole(const struct tcpcb* tp, tcp_seq* seq, uint32_t* len)
{
    tcp_seq start = SEQ_GT(tp->sack_rxmit, tp->snd_una) ? tp->sack_rxmit : tp->snd_una;
    for (uint8_t i = 0; i < tp->sack_count; i++) {
        const struct tcp_sack_block* b = &tp->sack_blocks[i];
        if (SEQ_LT(start, b->start)) {
            *seq = start;
            *len = b->start - start;
            return true;
        }
        if (SEQ_LT(start, b->end))
            start = b->end;
    }
    return false;
}

uint8_t tcp_sack_build_blocks(const struct tcpcb* tp, struct tcp_sack_block* out, const uint8_t max)
{
    uint8_t n = 0;
    for (const struct tcp_reassemble_entry* e = tp->reassemble_queue; e; e = e->next) {
        const tcp_seq end = e->seq + e->len;
        if (!e->len)
            continue;
        if (n && out[n - 1].end == e->seq) {
            out[n - 1].end = end;
            continue;
        }
        if (n == max)
            break;
        out[n++] = (struct tcp_sack_block){ e->seq, end };
    }
    return n;
}
