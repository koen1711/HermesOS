#include <drivers/net/arp/arp.h>

#include "drivers/net/core/netbuf.h"
#include "drivers/net/core/netif.h"
#include "drivers/net/core/netisr.h"
#include "drivers/net/core/byteorder.h"
#include "drivers/timer/callout.h"
#include "hardware/memory/alloc.h"
#include "os/errno.h"

#define ARP_QUEUE_LEN      64
#define ARP_TIMER_MS       250
#define ARP_RETRY_MS       1000

struct arp_cache {
    struct dlist entries;
};

static spinlock_t arp_lock;
static struct callout arp_callout;

static struct arp_cache* arp_cache_of(const struct netif* nif)
{
    return nif ? nif->arp_cache : nullptr;
}

static bool arp_hw_eq(const uint8_t* a, const uint8_t* b)
{
    for (int i = 0; i < ETH_ADDR_LEN; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

static void arp_free_pending(struct arp_entry* e)
{
    while (e->pending) {
        struct netbuf* next = e->pending->next_pkt;
        e->pending->next_pkt = nullptr;
        netbuf_free_chain(e->pending);
        e->pending = next;
    }
    e->pending_count = 0;
}

static struct netbuf* arp_take_pending(struct arp_entry* e)
{
    struct netbuf* list = e->pending;
    e->pending = nullptr;
    e->pending_count = 0;
    return list;
}

static void arp_send_pending(struct netif* nif, struct netbuf* list, const uint8_t* hw)
{
    while (list) {
        struct netbuf* next = list->next_pkt;
        list->next_pkt = nullptr;
        eth_output_frame(nif, list, hw, ETHERTYPE_IPV4);
        list = next;
    }
}

static int arp_send(struct netif* nif, const uint16_t op, const struct in_addr spa,
                    const uint8_t* tha, const struct in_addr tpa, const uint8_t* eth_dst)
{
    struct netbuf* nb = netbuf_alloc(true);
    if (!nb)
        return -ENOBUFS;

    struct arp_header ah;
    ah.hardware = htons(ARP_HTYPE_ETHERNET);
    ah.protocol = htons(ETHERTYPE_IPV4);
    ah.hardware_len = ETH_ADDR_LEN;
    ah.protocol_len = sizeof(struct in_addr);
    ah.op = htons(op);
    memcpy(ah.sha, nif->hw_addr, ETH_ADDR_LEN);
    ah.spa = spa;
    if (tha)
        memcpy(ah.tha, tha, ETH_ADDR_LEN);
    else
        memset(ah.tha, 0, ETH_ADDR_LEN);
    ah.tpa = tpa;

    if (!netbuf_append(nb, &ah, sizeof(ah))) {
        netbuf_free_chain(nb);
        return -ENOBUFS;
    }
    return eth_output_frame(nif, nb, eth_dst, ETHERTYPE_ARP);
}

static struct in_addr arp_source(struct netif* nif)
{
    const struct ipv4_ifaddr* ia = ipv4_addr_for_netif(nif);
    return ia ? ia->addr : (struct in_addr){ INADDR_ANY };
}

static bool arp_is_ours(const struct netif* nif, const struct in_addr ip)
{
    const struct ipv4_ifaddr* ia = ipv4_addr_find(ip);
    return ia && ia->nif == nif;
}

static struct arp_entry* arp_find(const struct arp_cache* cache, const struct in_addr ip)
{
    for (struct dlist_node* n = dlist_first(&cache->entries); n; n = dlist_next(n)) {
        const auto e = dlist_entry(n, struct arp_entry, link);
        if (e->ip.s_addr == ip.s_addr)
            return e;
    }
    return nullptr;
}

static void arp_entry_update(struct arp_entry* e, const uint8_t* hw, const enum arp_state state)
{
    if (hw)
        memcpy(e->hw, hw, ETH_ADDR_LEN);
    e->state = state;
    e->tries = 0;
    e->expires = state == ARP_REACHABLE ? ticks_get() + ticks_from_ms(ARP_REACHABLE_MS) : 0;
}

void arp_initialize()
{
    spinlock_init(&arp_lock, "arp");
    netisr_register(NETISR_ARP, arp_input, ARP_QUEUE_LEN);
    callout_init(&arp_callout);
    callout_reset(&arp_callout, ticks_from_ms(ARP_TIMER_MS), arp_timer, nullptr);
}

void arp_ifattach(struct netif* nif)
{
    struct arp_cache* cache = malloc(sizeof(struct arp_cache));
    if (!cache)
        return;
    dlist_init(&cache->entries);
    nif->arp_cache = cache;
}

void arp_ifdetach(struct netif* nif)
{
    struct arp_cache* cache = arp_cache_of(nif);
    if (!cache)
        return;
    uint64_t flags;
    spinlock_acquire_irqsave(&arp_lock, &flags);
    while (!dlist_empty(&cache->entries)) {
        const auto e = dlist_entry(dlist_first(&cache->entries), struct arp_entry, link);
        dlist_remove(&cache->entries, &e->link);
        arp_free_pending(e);
        free(e);
    }
    nif->arp_cache = nullptr;
    spinlock_release_irqrestore(&arp_lock, flags);
    free(cache);
}

void arp_input(struct netbuf* nb)
{
    struct netif* nif = nb->rcvif;
    if (!nif || !arp_cache_of(nif)) {
        netbuf_free_chain(nb);
        return;
    }
    nb = netbuf_pullup(nb, sizeof(struct arp_header));
    if (!nb)
        return;

    struct arp_header ah;
    netbuf_copydata(nb, 0, sizeof(ah), &ah);
    netbuf_free_chain(nb);

    if (ntohs(ah.hardware) != ARP_HTYPE_ETHERNET || ntohs(ah.protocol) != ETHERTYPE_IPV4 ||
        ah.hardware_len != ETH_ADDR_LEN || ah.protocol_len != sizeof(struct in_addr))
        return;
    const uint16_t op = ntohs(ah.op);
    if (op != ARP_OP_REQUEST && op != ARP_OP_REPLY)
        return;

    if (arp_hw_eq(ah.sha, nif->hw_addr) || eth_addr_is_multicast(ah.sha))
        return;

    const bool for_us = arp_is_ours(nif, ah.tpa);
    const bool learn = ah.spa.s_addr != INADDR_ANY && !ipv4_is_multicast(ah.spa)
                       && ah.spa.s_addr != INADDR_BROADCAST;

    struct netbuf* pending = nullptr;
    uint8_t hw[ETH_ADDR_LEN];
    memcpy(hw, ah.sha, ETH_ADDR_LEN);

    if (learn) {
        uint64_t flags;
        spinlock_acquire_irqsave(&arp_lock, &flags);
        const struct arp_cache* cache = arp_cache_of(nif);
        struct arp_entry* e = arp_find(cache, ah.spa);

        if (e && e->state != ARP_STATIC) {
            arp_entry_update(e, ah.sha, ARP_REACHABLE);
            pending = arp_take_pending(e);
        } else if (!e && for_us) {
            spinlock_release_irqrestore(&arp_lock, flags);
            arp_cache_add(nif, ah.spa, ah.sha, ARP_REACHABLE);
            spinlock_acquire_irqsave(&arp_lock, &flags);
        }
        spinlock_release_irqrestore(&arp_lock, flags);
    }
    arp_send_pending(nif, pending, hw);

    if (op == ARP_OP_REQUEST && for_us)
        arp_send(nif, ARP_OP_REPLY, ah.tpa, ah.sha, ah.spa, ah.sha);
}

int arp_resolve(struct netif* nif, const struct in_addr ip, struct netbuf* nb, uint8_t* out_hw)
{
    if (ipv4_is_broadcast(nif, ip)) {
        memcpy(out_hw, eth_broadcast_addr, ETH_ADDR_LEN);
        return 0;
    }
    if (ipv4_is_multicast(ip)) {
        eth_map_multicast4(&ip, out_hw);
        return 0;
    }

    struct arp_cache* cache = arp_cache_of(nif);
    if (!cache) {
        if (nb)
            netbuf_free_chain(nb);
        return -ENETDOWN;
    }

    uint64_t flags;
    spinlock_acquire_irqsave(&arp_lock, &flags);
    struct arp_entry* e = arp_find(cache, ip);
    if (e && e->state != ARP_INCOMPLETE) {
        memcpy(out_hw, e->hw, ETH_ADDR_LEN);
        spinlock_release_irqrestore(&arp_lock, flags);
        return 0;
    }

    bool send_request = false;
    if (!e) {
        e = malloc(sizeof(struct arp_entry));
        if (!e) {
            spinlock_release_irqrestore(&arp_lock, flags);
            if (nb)
                netbuf_free_chain(nb);
            return -ENOMEM;
        }
        memset(e, 0, sizeof(struct arp_entry));
        e->ip = ip;
        e->state = ARP_INCOMPLETE;
        e->tries = 1;
        e->expires = ticks_get() + ticks_from_ms(ARP_RETRY_MS);
        dlist_insert_tail(&cache->entries, &e->link);
        send_request = true;    // retries come from arp_timer
    }

    if (nb) {
        // keep the newest packets, like FreeBSD's la_hold
        if (e->pending_count >= ARP_MAX_PENDING) {
            struct netbuf* oldest = e->pending;
            e->pending = oldest->next_pkt;
            oldest->next_pkt = nullptr;
            netbuf_free_chain(oldest);
            e->pending_count--;
        }
        nb->next_pkt = nullptr;
        struct netbuf** tail = &e->pending;
        while (*tail)
            tail = &(*tail)->next_pkt;
        *tail = nb;
        e->pending_count++;
    }
    spinlock_release_irqrestore(&arp_lock, flags);

    if (send_request)
        arp_request(nif, arp_source(nif), ip);
    return -EINPROGRESS;
}

int arp_request(struct netif* nif, const struct in_addr src, const struct in_addr target)
{
    return arp_send(nif, ARP_OP_REQUEST, src, nullptr, target, eth_broadcast_addr);
}

int arp_announce(struct netif* nif, const struct in_addr ip)
{
    return arp_send(nif, ARP_OP_REQUEST, ip, nullptr, ip, eth_broadcast_addr);
}

int arp_probe(struct netif* nif, const struct in_addr ip)
{
    return arp_send(nif, ARP_OP_REQUEST, (struct in_addr){ INADDR_ANY }, nullptr, ip, eth_broadcast_addr);
}

struct arp_entry* arp_cache_add(struct netif* nif, const struct in_addr ip, const uint8_t* hw, const enum arp_state state)
{
    struct arp_cache* cache = arp_cache_of(nif);
    if (!cache)
        return nullptr;

    uint64_t flags;
    spinlock_acquire_irqsave(&arp_lock, &flags);
    struct arp_entry* e = arp_find(cache, ip);
    if (!e) {
        e = malloc(sizeof(struct arp_entry));
        if (!e) {
            spinlock_release_irqrestore(&arp_lock, flags);
            return nullptr;
        }
        memset(e, 0, sizeof(struct arp_entry));
        e->ip = ip;
        dlist_insert_tail(&cache->entries, &e->link);
    }
    arp_entry_update(e, hw, state);
    struct netbuf* pending = state != ARP_INCOMPLETE ? arp_take_pending(e) : nullptr;
    uint8_t out[ETH_ADDR_LEN];
    memcpy(out, e->hw, ETH_ADDR_LEN);
    spinlock_release_irqrestore(&arp_lock, flags);

    arp_send_pending(nif, pending, out);
    return e;
}

struct arp_entry* arp_cache_lookup(struct netif* nif, const struct in_addr ip)
{
    struct arp_cache* cache = arp_cache_of(nif);
    if (!cache)
        return nullptr;
    uint64_t flags;
    spinlock_acquire_irqsave(&arp_lock, &flags);
    struct arp_entry* e = arp_find(cache, ip);
    spinlock_release_irqrestore(&arp_lock, flags);
    return e;
}

void arp_cache_remove(struct netif* nif, const struct in_addr ip)
{
    struct arp_cache* cache = arp_cache_of(nif);
    if (!cache)
        return;
    uint64_t flags;
    spinlock_acquire_irqsave(&arp_lock, &flags);
    struct arp_entry* e = arp_find(cache, ip);
    if (e) {
        dlist_remove(&cache->entries, &e->link);
        arp_free_pending(e);
        free(e);
    }
    spinlock_release_irqrestore(&arp_lock, flags);
}

void arp_cache_flush(const struct netif* nif)
{
    struct arp_cache* cache = arp_cache_of(nif);
    if (!cache)
        return;
    uint64_t flags;
    spinlock_acquire_irqsave(&arp_lock, &flags);
    struct dlist_node* n = dlist_first(&cache->entries);
    while (n) {
        struct dlist_node* next = dlist_next(n);
        struct arp_entry* e = dlist_entry(n, struct arp_entry, link);
        if (e->state != ARP_STATIC) {
            dlist_remove(&cache->entries, n);
            arp_free_pending(e);
            free(e);
        }
        n = next;
    }
    spinlock_release_irqrestore(&arp_lock, flags);
}

void arp_timer(void* arg)
{
    (void)arg;
    const uint64_t now = ticks_get();

    for (struct netif* nif = netif_first(); nif; nif = netif_next(nif)) {
        struct arp_cache* cache = arp_cache_of(nif);
        if (!cache)
            continue;

        struct in_addr retry[8];
        int nretry = 0;

        uint64_t flags;
        spinlock_acquire_irqsave(&arp_lock, &flags);
        struct dlist_node* n = dlist_first(&cache->entries);
        while (n) {
            struct dlist_node* next = dlist_next(n);
            struct arp_entry* e = dlist_entry(n, struct arp_entry, link);
            if (e->state == ARP_INCOMPLETE && e->expires <= now) {
                if (e->tries >= ARP_MAX_TRIES) {
                    dlist_remove(&cache->entries, n);
                    arp_free_pending(e);
                    free(e);
                } else if (nretry < 8) {
                    e->tries++;
                    e->expires = now + ticks_from_ms(ARP_RETRY_MS);
                    retry[nretry++] = e->ip;
                }
            } else if (e->state == ARP_REACHABLE && e->expires <= now) {
                e->state = ARP_STALE;
            }
            n = next;
        }
        spinlock_release_irqrestore(&arp_lock, flags);

        const struct in_addr src = arp_source(nif);
        for (int i = 0; i < nretry; i++)
            arp_request(nif, src, retry[i]);
    }

    callout_reset(&arp_callout, ticks_from_ms(ARP_TIMER_MS), arp_timer, nullptr);
}
