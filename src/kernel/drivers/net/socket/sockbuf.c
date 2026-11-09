#include <drivers/net/socket/sockbuf.h>

#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netif.h>
#include <drivers/net/core/netisr.h>
#include <drivers/net/socket/socket.h>
#include <drivers/timer/callout.h>
#include <hardware/interrupts/irq.h>
#include <drivers/scheduler/process.h>
#include <hardware/memory/alloc.h>
#include <hardware/smp/smp.h>
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
    sb->head = sb->tail = sb->last = nullptr;
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
    sb->head = sb->tail = sb->last = nullptr;
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
    struct netbuf* last = nb;
    while (last->next)
        last = last->next;
    uint64_t flags;
    spinlock_acquire_irqsave(&sb->lock, &flags);
    if (sb->tail) {
        netbuf_cat(sb->last, nb);
    } else {
        nb->next_pkt = nullptr;
        sb->head = sb->tail = nb;
    }
    sb->last = last;
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
    struct netbuf* last = rec;
    while (last->next)
        last = last->next;

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
    sb->last = last;
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
    if (!sb->head)
        sb->last = nullptr;
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
            sb->tail = sb->last = nullptr;
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

bool sockbuf_writable(const struct sockbuf* sb)
{
    const size_t space = sockbuf_space(sb);
    return space && space >= sb->hiwat / 3;
}

size_t sockbuf_copy(const struct sockbuf* sb, size_t offset, size_t len, void* out)
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
    return dst - (uint8_t*)out;
}

static int sockbuf_wait_id(const struct sockbuf* sb)
{
    return (int)(((uintptr_t)sb >> 4) & 0x3FFFFFFF) | 0x40000000;
}

static bool sockbuf_can_sleep()
{
    const process_t* current = scheduler_get()->current;
    return current && current->pid != IDLE_PROC_PID && !irq_in_interrupt();
}

static void sockbuf_poll_once()
{
    netif_tx_flush();
    const uint64_t rflags = irq_save();
    irq_restore(rflags);
    if (rflags & 0x200)
        asm volatile("hlt");
    else
        netif_poll_all();
    netisr_poll();
    callout_poll();
}

static bool sockbuf_drain_netisr()
{
    if (!netisr_pending())
        return false;
    if (net_lock_held())
        return netisr_poll_pending();
    struct sock_lock_saved socks;
    sock_lock_save(&socks);
    const bool did = netisr_poll_pending();
    sock_lock_restore(&socks);
    return did;
}

int sockbuf_timedwait(struct sockbuf* sb, const uint64_t timeout)
{
    if (sockbuf_drain_netisr())
        return 0;
    if (!sockbuf_can_sleep()) {
        __atomic_or_fetch(&sb->flags, SB_WAIT, __ATOMIC_RELAXED);
        struct sock_lock_saved socks;
        sock_lock_save(&socks);
        sockbuf_poll_once();
        sock_lock_restore(&socks);
        return 0;
    }

    const uint64_t flags = irq_save();
    __atomic_or_fetch(&sb->flags, SB_WAIT, __ATOMIC_SEQ_CST);
    int err = timeout ? process_sleep_timeout(sockbuf_wait_id(sb), timeout)
                      : process_sleep_interruptible(sockbuf_wait_id(sb));
    irq_restore(flags);
    if (err == -ETIMEDOUT)
        err = 0;

    return err == -EINTR && !timeout ? -ERESTARTSYS : err;
}

int sockbuf_wait(struct sockbuf* sb)
{
    return sockbuf_timedwait(sb, 0);
}

void sockbuf_wakeup(struct sockbuf* sb)
{
    if (!(__atomic_load_n(&sb->flags, __ATOMIC_ACQUIRE) & SB_WAIT))
        return;
    __atomic_and_fetch(&sb->flags, ~SB_WAIT, __ATOMIC_SEQ_CST);
    process_wakeup(sockbuf_wait_id(sb));
}
