#include <drivers/net/socket/sockbuf.h>

#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netif.h>
#include <drivers/net/core/netisr.h>
#include <drivers/timer/callout.h>
#include <hardware/interrupts/irq.h>
#include <scheduler/process.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

static bool sockbuf_is_addr(const struct netbuf* nb)
{
    return nb->flags & NETBUF_SOCKADDR;
}

static size_t sockbuf_chain_len(const struct netbuf* nb)
{
    size_t n = 0;
    for (; nb; nb = nb->next)
        if (!sockbuf_is_addr(nb))
            n += nb->len;
    return n;
}

void sockbuf_init(struct sockbuf* sb, const size_t hiwat)
{
    sb->head = sb->tail = nullptr;
    sb->cc = 0;
    sb->hiwat = hiwat;
    sb->lowat = 1;
    sb->flags = 0;
    spinlock_init(&sb->lock, "sockbuf");
}

void sockbuf_release(struct sockbuf* sb)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&sb->lock, &flags);
    struct netbuf* rec = sb->head;
    sb->head = sb->tail = nullptr;
    sb->cc = 0;
    spinlock_release_irqrestore(&sb->lock, flags);

    while (rec) {
        struct netbuf* next = rec->next_pkt;
        netbuf_free_chain(rec);
        rec = next;
    }
}

void sockbuf_append(struct sockbuf* sb, struct netbuf* nb)
{
    if (!nb)
        return;
    const size_t len = sockbuf_chain_len(nb);
    uint64_t flags;
    spinlock_acquire_irqsave(&sb->lock, &flags);
    if (sb->tail) {
        netbuf_cat(sb->tail, nb);
    } else {
        nb->next_pkt = nullptr;
        sb->head = sb->tail = nb;
    }
    sb->cc += len;
    spinlock_release_irqrestore(&sb->lock, flags);
}

bool sockbuf_append_addr(struct sockbuf* sb, const struct sockaddr* from, const socklen_t from_len, struct netbuf* nb)
{
    const size_t len = nb ? sockbuf_chain_len(nb) : 0;
    if (from_len > NETBUF_SIZE - NETBUF_HEADROOM)
        return false;

    struct netbuf* rec = netbuf_alloc(true);
    if (!rec)
        return false;
    if (from && from_len) {
        memcpy(rec->data, (void*)from, from_len);
        rec->len = from_len;
    }
    rec->flags |= NETBUF_SOCKADDR;
    rec->next = nb;
    rec->next_pkt = nullptr;
    if (nb)
        nb->flags &= ~NETBUF_PKTHDR;

    uint64_t flags;
    spinlock_acquire_irqsave(&sb->lock, &flags);
    if (len > sockbuf_space(sb) || (sb->flags & SB_CANTMORE)) {
        spinlock_release_irqrestore(&sb->lock, flags);
        rec->next = nullptr;
        if (nb)
            nb->flags |= NETBUF_PKTHDR;
        netbuf_free(rec);
        return false;
    }
    if (sb->tail)
        sb->tail->next_pkt = rec;
    else
        sb->head = rec;
    sb->tail = rec;
    sb->cc += len;
    spinlock_release_irqrestore(&sb->lock, flags);
    return true;
}

void sockbuf_drop(struct sockbuf* sb, size_t len)
{
    struct netbuf* freed = nullptr;
    uint64_t flags;
    spinlock_acquire_irqsave(&sb->lock, &flags);
    while (len && sb->head) {
        struct netbuf* b = sb->head;
        if (!sockbuf_is_addr(b) && b->len > len) {
            b->data += len;
            b->len -= len;
            sb->cc -= len;
            break;
        }
        if (!sockbuf_is_addr(b)) {
            len -= b->len;
            sb->cc -= b->len;
        }

        struct netbuf* rest = b->next;
        if (rest) {
            rest->next_pkt = b->next_pkt;
            if (sb->tail == b)
                sb->tail = rest;
            sb->head = rest;
        } else {
            sb->head = b->next_pkt;
            if (sb->tail == b)
                sb->tail = nullptr;
        }
        b->next = nullptr;
        b->next_pkt = freed;
        freed = b;
    }
    spinlock_release_irqrestore(&sb->lock, flags);

    while (freed) {
        struct netbuf* next = freed->next_pkt;
        netbuf_free(freed);
        freed = next;
    }
}

void sockbuf_drop_record(struct sockbuf* sb)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&sb->lock, &flags);
    struct netbuf* rec = sb->head;
    if (rec) {
        sb->head = rec->next_pkt;
        if (!sb->head)
            sb->tail = nullptr;
        sb->cc -= sockbuf_chain_len(rec);
        rec->next_pkt = nullptr;
    }
    spinlock_release_irqrestore(&sb->lock, flags);
    if (rec)
        netbuf_free_chain(rec);
}

size_t sockbuf_space(const struct sockbuf* sb)
{
    return sb->cc < sb->hiwat ? sb->hiwat - sb->cc : 0;
}

void sockbuf_copy(const struct sockbuf* sb, size_t offset, size_t len, void* out)
{
    uint8_t* dst = out;
    for (const struct netbuf* rec = sb->head; rec && len; rec = rec->next_pkt) {
        for (const struct netbuf* b = rec; b && len; b = b->next) {
            if (sockbuf_is_addr(b))
                continue;
            if (offset >= b->len) {
                offset -= b->len;
                continue;
            }
            const size_t n = b->len - offset < len ? b->len - offset : len;
            memcpy(dst, b->data + offset, n);
            dst += n;
            len -= n;
            offset = 0;
        }
    }
}

// A process sleeps on the sockbuf (FreeBSD sbwait -> msleep). The kernel itself (no process
// yet, the idle loop, interrupt context) can't sleep and polls the NICs, queues and timers instead.
static int sockbuf_wait_id(const struct sockbuf* sb)
{
    // positive and above every pid, INIT_PROC_ID and WAIT_KEY_INT; a collision is only a spurious wakeup
    return (int)(((uintptr_t)sb >> 4) & 0x3FFFFFFF) | 0x40000000;
}

static bool sockbuf_can_sleep()
{
    const process_t* current = scheduler_get()->current;
    return current && current->pid != IDLE_PROC_PID && !irq_in_interrupt();
}

static void sockbuf_poll_once()
{
    const uint64_t rflags = irq_save();
    irq_restore(rflags);
    if (rflags & 0x200)     // IF
        asm volatile("hlt");
    else
        netif_poll_all();
    netisr_poll();
    callout_poll();
}

static void sockbuf_timeout(void* arg)
{
    sockbuf_wakeup(arg);
}

// Callers re-check their condition after every return: wakeups can be spurious
int sockbuf_timedwait(struct sockbuf* sb, const uint64_t timeout)
{
    // packets still queued (loopback, or what the caller just sent) may be what it waits for
    if (netisr_poll_pending())
        return 0;
    if (!sockbuf_can_sleep()) {
        sb->flags |= SB_WAIT;
        sockbuf_poll_once();
        return 0;
    }

    // interrupts stay off from the caller's check until we are on the wait list: no lost wakeup
    const uint64_t flags = irq_save();
    struct callout timer;
    callout_init(&timer);
    if (timeout)
        callout_reset(&timer, timeout, sockbuf_timeout, sb);
    sb->flags |= SB_WAIT;
    process_sleep(sockbuf_wait_id(sb));
    callout_stop(&timer);
    irq_restore(flags);
    return 0;
}

int sockbuf_wait(struct sockbuf* sb)
{
    return sockbuf_timedwait(sb, 0);
}

void sockbuf_wakeup(struct sockbuf* sb)
{
    if (!(sb->flags & SB_WAIT))
        return;
    sb->flags &= ~SB_WAIT;
    process_wakeup(sockbuf_wait_id(sb));
}
