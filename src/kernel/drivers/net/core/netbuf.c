#include <drivers/net/core/netbuf.h>

#include "hardware/interrupts/panic.h"
#include "hardware/memory/alloc.h"

void netbuf_initialize()
{

}

static struct netbuf* netbuf_alloc_sized(const size_t size, const bool pkthdr)
{
    struct netbuf* nb = malloc(sizeof(struct netbuf) + size);
    if (!nb)
        return nullptr;
    memset(nb, 0, sizeof(struct netbuf));
    nb->buf = (uint8_t*)(nb + 1);
    nb->buf_size = size;
    nb->data = nb->buf + (pkthdr ? NETBUF_HEADROOM : 0);
    nb->flags = pkthdr ? NETBUF_PKTHDR : 0;
    return nb;
}

struct netbuf* netbuf_alloc(bool pkthdr)
{
    return netbuf_alloc_sized(NETBUF_SIZE, pkthdr);
}

struct netbuf* netbuf_alloc_cluster(bool pkthdr)
{
    return netbuf_alloc_sized(NETBUF_CLUSTER_SIZE, pkthdr);
}

struct netbuf* netbuf_alloc_packet(const size_t len)
{
    const bool small = len + NETBUF_HEADROOM <= NETBUF_SIZE;
    struct netbuf* head = small ? netbuf_alloc(true) : netbuf_alloc_cluster(true);
    if (!head)
        return nullptr;

    size_t room = head->buf_size - NETBUF_HEADROOM;
    struct netbuf* last = head;
    while (room < len) {
        struct netbuf* nb = netbuf_alloc_cluster(false);
        if (!nb) {
            netbuf_free_chain(head);
            return nullptr;
        }
        last->next = nb;
        last = nb;
        room += nb->buf_size;
    }
    return head;
}

void netbuf_free(struct netbuf* nb)
{
    free(nb);
}

void netbuf_free_chain(struct netbuf* nb)
{
    while (nb)
    {
        struct netbuf* next = nb->next;
        netbuf_free(nb);
        nb = next;
    }
}

static size_t netbuf_trailing_space(const struct netbuf* nb)
{
    return (size_t)((nb->buf + nb->buf_size) - (nb->data + nb->len));
}

static void netbuf_move_pkthdr(struct netbuf* to, struct netbuf* from)
{
    to->flags = (to->flags & NETBUF_EXT) | (from->flags & ~NETBUF_EXT) | NETBUF_PKTHDR;
    to->next_pkt = from->next_pkt;
    to->pkt_len = from->pkt_len;
    to->rcvif = from->rcvif;
    to->csum_flags = from->csum_flags;
    to->l3_offset = from->l3_offset;
    to->l4_offset = from->l4_offset;
    from->flags &= ~NETBUF_PKTHDR;
    from->next_pkt = nullptr;
}

static struct netbuf* netbuf_alloc_fit(const size_t len, const bool pkthdr)
{
    return len <= NETBUF_SIZE ? netbuf_alloc(pkthdr) : netbuf_alloc_cluster(pkthdr);
}


struct netbuf* netbuf_prepend(struct netbuf* nb, const size_t len)
{
    if ((size_t)(nb->data - nb->buf) >= len) {
        nb->data -= len;
        nb->len += len;
        nb->pkt_len += len;
        return nb;
    }

    struct netbuf* head = len <= NETBUF_CLUSTER_SIZE ? netbuf_alloc_fit(len, false) : nullptr;
    if (!head) {
        netbuf_free_chain(nb);
        return nullptr;
    }
    netbuf_move_pkthdr(head, nb);

    head->data = head->buf + head->buf_size - len;
    head->len = len;
    head->pkt_len += len;
    head->next = nb;
    return head;
}

void netbuf_adj(struct netbuf* nb, const ssize_t len)
{
    if (len >= 0) {
        size_t left = (size_t)len;
        for (struct netbuf* b = nb; b && left; b = b->next) {
            const size_t n = left < b->len ? left : b->len;
            b->data += n;
            b->len -= n;
            left -= n;
        }
        nb->pkt_len -= (size_t)len - left;
        return;
    }

    size_t total = 0;
    for (const struct netbuf* b = nb; b; b = b->next)
        total += b->len;
    const size_t trim = (size_t)-len;
    size_t keep = trim < total ? total - trim : 0;

    struct netbuf* b = nb;
    for (;;) {
        if (b->len >= keep || !b->next) {
            b->len = keep < b->len ? keep : b->len;
            break;
        }
        keep -= b->len;
        b = b->next;
    }
    netbuf_free_chain(b->next);
    b->next = nullptr;
    nb->pkt_len = total - (trim < total ? trim : total);
}

struct netbuf* netbuf_pullup(struct netbuf* nb, const size_t len)
{
    if (nb->len >= len)
        return nb;
    if (len > NETBUF_CLUSTER_SIZE || nb->pkt_len < len)
        goto fail;

    struct netbuf* head = nb;
    if ((size_t)(nb->buf + nb->buf_size - nb->data) < len) {
        head = netbuf_alloc_fit(len, false);
        if (!head)
            goto fail;
        netbuf_move_pkthdr(head, nb);
        memcpy(head->data, nb->data, nb->len);
        head->len = nb->len;
        head->next = nb->next;
        netbuf_free(nb);
    }

    struct netbuf* rest = head->next;
    while (head->len < len) {
        const size_t want = len - head->len;
        const size_t n = want < rest->len ? want : rest->len;
        memcpy(head->data + head->len, rest->data, n);
        head->len += n;
        rest->data += n;
        rest->len -= n;
        if (rest->len == 0) {
            struct netbuf* next = rest->next;
            netbuf_free(rest);
            rest = next;
        }
    }
    head->next = rest;
    return head;

fail:
    netbuf_free_chain(nb);
    return nullptr;
}

bool netbuf_append(struct netbuf* nb, const void* data, const size_t len)
{
    const uint8_t* src = data;
    size_t left = len;

    struct netbuf* last = nb;
    while (last->next)
        last = last->next;

    size_t n = netbuf_trailing_space(last);
    if (n > left)
        n = left;
    memcpy(last->data + last->len, src, n);
    last->len += n;
    src += n;
    left -= n;

    while (left) {
        struct netbuf* b = netbuf_alloc_cluster(false);
        if (!b)
            break;
        n = netbuf_trailing_space(b);
        if (n > left)
            n = left;
        memcpy(b->data, src, n);
        b->len = n;
        last->next = b;
        last = b;
        src += n;
        left -= n;
    }

    if (nb->flags & NETBUF_PKTHDR)
        nb->pkt_len += len - left;

    return left == 0;
}

void netbuf_copydata(const struct netbuf* nb, size_t offset, size_t len, void* out)
{
    uint8_t* dst = out;
    while (nb && offset >= nb->len && offset) {
        offset -= nb->len;
        nb = nb->next;
    }
    for (; nb && len; nb = nb->next) {
        const size_t avail = nb->len - offset;
        const size_t n = len < avail ? len : avail;
        memcpy(dst, nb->data + offset, n);
        dst += n;
        len -= n;
        offset = 0;
    }
}

bool netbuf_copyback(struct netbuf* nb, size_t offset, size_t len, const void* data)
{
    const uint8_t* src = data;
    struct netbuf* b = nb;
    while (b && offset > b->len) {
        offset -= b->len;
        b = b->next;
    }
    if (!b)
        return false;

    for (; b && len; b = b->next) {
        const size_t avail = b->len - offset;
        const size_t n = len < avail ? len : avail;
        memcpy(b->data + offset, src, n);
        src += n;
        len -= n;
        offset = 0;
    }
    return len == 0 || netbuf_append(nb, src, len);
}

struct netbuf* netbuf_dup(const struct netbuf* nb)
{
    struct netbuf* copy = netbuf_alloc_fit(nb->pkt_len + NETBUF_HEADROOM, true);
    if (!copy)
        return nullptr;

    for (const struct netbuf* b = nb; b; b = b->next) {
        if (!netbuf_append(copy, b->data, b->len)) {
            netbuf_free_chain(copy);
            return nullptr;
        }
    }
    copy->flags |= nb->flags & ~NETBUF_EXT;
    copy->rcvif = nb->rcvif;
    copy->csum_flags = nb->csum_flags;
    copy->l3_offset = nb->l3_offset;
    copy->l4_offset = nb->l4_offset;
    return copy;
}

struct netbuf* netbuf_split(struct netbuf* nb, size_t offset)
{
    const size_t total = nb->pkt_len;
    if (offset > total)
        return nullptr;

    struct netbuf* b = nb;
    size_t k = offset;
    while (k > b->len) {
        k -= b->len;
        b = b->next;
    }

    struct netbuf* tail = netbuf_alloc_fit(b->len - k + NETBUF_HEADROOM, true);
    if (!tail)
        return nullptr;

    if (!netbuf_append(tail, b->data + k, b->len - k)) {
        netbuf_free_chain(tail);
        return nullptr;
    }
    struct netbuf* last = tail;
    while (last->next)
        last = last->next;
    last->next = b->next;

    b->len = k;
    b->next = nullptr;

    tail->rcvif = nb->rcvif;
    tail->pkt_len = total - offset;
    nb->pkt_len = offset;
    return tail;
}

void netbuf_cat(struct netbuf* nb, struct netbuf* tail)
{
    if (!tail)
        return;
    struct netbuf* last = nb;
    while (last->next)
        last = last->next;
    last->next = tail;
    tail->flags &= ~NETBUF_PKTHDR;
    tail->next_pkt = nullptr;
}
