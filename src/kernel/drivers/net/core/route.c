#include <drivers/net/core/route.h>

#include <drivers/net/core/byteorder.h>
#include <drivers/timer/callout.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>
#include <utils/sync/spinlock.h>

static struct dlist routes4;
static struct dlist routes6;
static spinlock_t route_lock;

void route_initialize()
{
    dlist_init(&routes4);
    dlist_init(&routes6);
    spinlock_init(&route_lock, "route");
}

static uint32_t route_mask4(const uint8_t prefix_len)
{
    return htonl(prefix_len ? 0xFFFFFFFFu << (32 - prefix_len) : 0);
}

static struct route_entry* route_find4(const struct in_addr dst, const uint8_t prefix_len)
{
    for (struct dlist_node* n = dlist_first(&routes4); n; n = dlist_next(n)) {
        const auto r = dlist_entry(n, struct route_entry, link);
        if (r->prefix_len == prefix_len && r->dst4.s_addr == dst.s_addr)
            return r;
    }
    return nullptr;
}

int route_add4(struct in_addr dst, const uint8_t prefix_len, const struct in_addr gw, struct netif* nif, const uint32_t flags)
{
    if (prefix_len > 32 || !nif)
        return -EINVAL;
    dst.s_addr &= route_mask4(prefix_len);

    uint64_t irq;
    spinlock_acquire_irqsave(&route_lock, &irq);
    struct route_entry* r = route_find4(dst, prefix_len);
    if (!r) {
        r = malloc(sizeof(struct route_entry));
        if (!r) {
            spinlock_release_irqrestore(&route_lock, irq);
            return -ENOMEM;
        }
        memset(r, 0, sizeof(struct route_entry));
        dlist_insert_tail(&routes4, &r->link);
    }
    r->family = AF_INET;
    r->dst4 = dst;
    r->prefix_len = prefix_len;
    r->flags = flags | ROUTE_UP | (gw.s_addr != INADDR_ANY ? ROUTE_GATEWAY : 0)
               | (prefix_len == 32 ? ROUTE_HOST : 0);
    r->nexthop.nif = nif;
    r->nexthop.family = AF_INET;
    r->nexthop.gw4 = gw;
    r->nexthop.mtu = 0;
    spinlock_release_irqrestore(&route_lock, irq);
    return 0;
}

int route_del4(struct in_addr dst, const uint8_t prefix_len)
{
    dst.s_addr &= route_mask4(prefix_len);
    uint64_t irq;
    spinlock_acquire_irqsave(&route_lock, &irq);
    struct route_entry* r = route_find4(dst, prefix_len);
    if (r)
        dlist_remove(&routes4, &r->link);
    spinlock_release_irqrestore(&route_lock, irq);
    if (!r)
        return -ESRCH;
    free(r);
    return 0;
}

const struct route_nexthop* route_lookup4(const struct in_addr dst)
{
    const struct route_entry* best = nullptr;
    uint64_t irq;
    spinlock_acquire_irqsave(&route_lock, &irq);
    for (struct dlist_node* n = dlist_first(&routes4); n; n = dlist_next(n)) {
        const struct route_entry* r = dlist_entry(n, struct route_entry, link);
        if (!(r->flags & ROUTE_UP))
            continue;
        if ((dst.s_addr & route_mask4(r->prefix_len)) != r->dst4.s_addr)
            continue;
        if (!best || r->prefix_len > best->prefix_len)
            best = r;
    }
    spinlock_release_irqrestore(&route_lock, irq);
    return best ? &best->nexthop : nullptr;
}

int route_set_default4(const struct in_addr gw, struct netif* nif)
{
    return route_add4((struct in_addr){ INADDR_ANY }, 0, gw, nif, ROUTE_STATIC);
}

void route_flush_netif(const struct netif* nif)
{
    struct dlist* tables[] = { &routes4, &routes6 };
    uint64_t irq;
    spinlock_acquire_irqsave(&route_lock, &irq);
    for (int t = 0; t < 2; t++) {
        struct dlist_node* n = dlist_first(tables[t]);
        while (n) {
            struct dlist_node* next = dlist_next(n);
            struct route_entry* r = dlist_entry(n, struct route_entry, link);
            if (r->nexthop.nif == nif) {
                dlist_remove(tables[t], n);
                free(r);
            }
            n = next;
        }
    }
    spinlock_release_irqrestore(&route_lock, irq);
}

void route_timer()
{
    const uint64_t now = ticks_get();
    struct dlist* tables[] = { &routes4, &routes6 };
    uint64_t irq;
    spinlock_acquire_irqsave(&route_lock, &irq);
    for (int t = 0; t < 2; t++) {
        struct dlist_node* n = dlist_first(tables[t]);
        while (n) {
            struct dlist_node* next = dlist_next(n);
            struct route_entry* r = dlist_entry(n, struct route_entry, link);
            if (r->expires && r->expires <= now) {
                dlist_remove(tables[t], n);
                free(r);
            }
            n = next;
        }
    }
    spinlock_release_irqrestore(&route_lock, irq);
}
