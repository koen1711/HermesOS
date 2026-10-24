#include <drivers/net/socket/sockbuf.h>

#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netisr.h>
#include <hardware/interrupts/irq.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

// Records are linked through next_pkt, the data of one record through next. A datagram
// record starts with a NETBUF_SOCKADDR buffer holding the sender's address.

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

// Free everything in the buffer
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

// False (and nb still owned by the caller) when the datagram doesn't fit
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

// Remove len data bytes from the front, across records (stream sockets)
void sockbuf_drop(struct sockbuf* sb, size_t len)
{
    struct netbuf* freed = nullptr;     // used-up buffers, freed outside the lock
    uint64_t flags;
    spinlock_acquire_irqsave(&sb->lock, &flags);
    while (len && sb->head) {
        struct netbuf* b = sb->head;    // first buffer of the first record
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
        // the rest of the record (if any) becomes the record head
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

// Copy data bytes [offset, offset + len) without removing them; skips sender addresses
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

// Wait until something happens to the buffer. There is no scheduler yet, so this halts
// until the next interrupt and runs the deferred protocol work it queued. Returns
// -EWOULDBLOCK when interrupts are off, since nothing could ever wake us then.
// TODO: sleep on a wait queue once there are threads
int sockbuf_wait(struct sockbuf* sb)
{
    const uint64_t rflags = irq_save();
    irq_restore(rflags);
    if (!(rflags & 0x200))     // IF
        return -EWOULDBLOCK;
    sb->flags |= SB_WAIT;
    asm volatile("hlt");
    netisr_poll();
    return 0;
}

void sockbuf_wakeup(struct sockbuf* sb)
{
    sb->flags &= ~SB_WAIT;
}
