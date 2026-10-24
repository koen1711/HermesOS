#include <drivers/net/ipv6/mld6.h>

#include <drivers/net/core/netif.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/byteorder.h>
#include <drivers/net/core/random.h>
#include <drivers/net/ethernet/ethernet.h>
#include <drivers/net/ipv4/ipv4.h>
#include <drivers/net/ipv6/icmp6.h>
#include <drivers/timer/callout.h>
#include <hardware/memory/alloc.h>
#include <utils/sync/spinlock.h>
#include <os/errno.h>

#define MLD6_TIMER_MS          100
#define MLD6_ROBUSTNESS        2
#define MLD6_UNSOLICITED_MS    1000
#define MLD6_MAX_SEND          16

enum mld6_record_type
{
    MLD6_MODE_IS_EXCLUDE = 2,
    MLD6_CHANGE_TO_INCLUDE = 3,
    MLD6_CHANGE_TO_EXCLUDE = 4
};

struct mld6_group {
    struct dlist_node link;
    struct netif* nif;
    struct in6_addr addr;
    uint32_t refcount;
    uint8_t reports_left;
    uint8_t record_type;
    uint64_t report_at;
};

struct mld6_report_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t reserved;
    uint16_t records;
} __attribute__((packed));

struct mld6_record {
    uint8_t type;
    uint8_t aux_len;
    uint16_t sources;
    struct in6_addr group;
} __attribute__((packed));

static struct dlist groups;
static spinlock_t mld6_lock;
static struct callout mld6_callout;

static const struct in6_addr mld6_all_routers = {
    .s6_addr = { 0xFF, 0x02, [15] = 0x16 }
};

void mld6_initialize()
{
    dlist_init(&groups);
    spinlock_init(&mld6_lock, "mld6");
    callout_init(&mld6_callout);
    callout_reset(&mld6_callout, ticks_from_ms(MLD6_TIMER_MS), mld6_timer, nullptr);
}

static bool mld6_is_all_nodes(const struct in6_addr* group)
{
    static const struct in6_addr all_nodes = { .s6_addr = { 0xFF, 0x02, [15] = 1 } };
    return ipv6_addr_equal(group, &all_nodes);
}

static bool mld6_reportable(const struct in6_addr* group)
{
    return !mld6_is_all_nodes(group) && (group->s6_addr[1] & 0x0F) > 1;
}

static struct mld6_group* mld6_find(const struct netif* nif, const struct in6_addr* group)
{
    for (struct dlist_node* n = dlist_first(&groups); n; n = dlist_next(n)) {
        const auto g = dlist_entry(n, struct mld6_group, link);
        if (g->nif == nif && ipv6_addr_equal(&g->addr, group))
            return g;
    }
    return nullptr;
}

static uint64_t mld6_random_delay(const uint64_t max_ms)
{
    return 1 + ticks_from_ms(net_random32() % (max_ms + 1));
}

static int mld6_send_report(struct netif* nif, const struct in6_addr* group, const uint8_t record_type)
{
    static const uint8_t router_alert[8] = {
        IPPROTO_ICMPV6, 0,
        5, 2, 0, 0,
        1, 0,
    };
    constexpr size_t icmp_len = sizeof(struct mld6_report_hdr) + sizeof(struct mld6_record);
    struct netbuf* nb = netbuf_alloc_packet(sizeof(router_alert) + icmp_len);
    if (!nb)
        return -ENOBUFS;
    const struct mld6_report_hdr h = { .type = ICMP6_MLDV2_REPORT, .records = htons(1) };
    const struct mld6_record r = { .type = record_type, .group = *group };
    if (!netbuf_append(nb, router_alert, sizeof(router_alert)) || !netbuf_append(nb, &h, sizeof(h)) ||
        !netbuf_append(nb, &r, sizeof(r))) {
        netbuf_free_chain(nb);
        return -ENOBUFS;
    }

    const struct ipv6_ifaddr* ll = ipv6_addr_linklocal(nif);
    const bool have_ll = ll && !(ll->flags & (IPV6_ADDR_TENTATIVE | IPV6_ADDR_DUPLICATED));
    const struct in6_addr src = have_ll ? ll->addr : in6addr_any;
    const uint16_t sum = icmp6_checksum(nb, sizeof(router_alert), icmp_len, &src, &mld6_all_routers);
    const uint16_t net_sum = htons(sum);
    netbuf_copyback(nb, sizeof(router_alert) + 2, 2, &net_sum);

    const struct ipv6_options opts = { .hop_limit = 1, .nif = nif };
    return ipv6_output(nb, &src, &mld6_all_routers, IPV6_NH_HOPOPTS, &opts, have_ll ? 0 : IPV6_OUTPUT_UNSPEC_SRC);
}

static void mld6_set_filter(struct netif* nif, const struct in6_addr* group, const bool add)
{
    if (!(nif->flags & NETIF_MULTICAST) || !nif->ops || !nif->ops->ioctl)
        return;
    uint8_t mac[ETH_ADDR_LEN];
    eth_map_multicast6(group, mac);
    netif_ioctl(nif, add ? NETIF_IOCTL_ADD_MULTI : NETIF_IOCTL_DEL_MULTI, mac);
}

int mld6_join(struct netif* nif, const struct in6_addr* group)
{
    if (!ipv6_addr_is_multicast(group))
        return -EINVAL;
    uint64_t flags;
    spinlock_acquire_irqsave(&mld6_lock, &flags);
    struct mld6_group* g = mld6_find(nif, group);
    if (g) {
        g->refcount++;
        spinlock_release_irqrestore(&mld6_lock, flags);
        return 0;
    }
    g = malloc(sizeof(struct mld6_group));
    if (!g) {
        spinlock_release_irqrestore(&mld6_lock, flags);
        return -ENOMEM;
    }
    memset(g, 0, sizeof(struct mld6_group));
    g->nif = nif;
    g->addr = *group;
    g->refcount = 1;
    const bool report = mld6_reportable(group);
    if (report) {
        g->record_type = MLD6_CHANGE_TO_EXCLUDE;
        g->reports_left = MLD6_ROBUSTNESS - 1;
        g->report_at = ticks_get() + mld6_random_delay(MLD6_UNSOLICITED_MS);
    }
    dlist_insert_tail(&groups, &g->link);
    spinlock_release_irqrestore(&mld6_lock, flags);

    mld6_set_filter(nif, group, true);
    if (report)
        mld6_send_report(nif, group, MLD6_CHANGE_TO_EXCLUDE);
    return 0;
}

int mld6_leave(struct netif* nif, const struct in6_addr* group)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&mld6_lock, &flags);
    struct mld6_group* g = mld6_find(nif, group);
    if (!g) {
        spinlock_release_irqrestore(&mld6_lock, flags);
        return -ENOENT;
    }
    if (--g->refcount) {
        spinlock_release_irqrestore(&mld6_lock, flags);
        return 0;
    }
    dlist_remove(&groups, &g->link);
    spinlock_release_irqrestore(&mld6_lock, flags);

    if (mld6_reportable(group))
        mld6_send_report(nif, group, MLD6_CHANGE_TO_INCLUDE);
    mld6_set_filter(nif, group, false);
    free(g);
    return 0;
}

bool mld6_is_member(struct netif* nif, const struct in6_addr* group)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&mld6_lock, &flags);
    const bool member = mld6_find(nif, group) != nullptr;
    spinlock_release_irqrestore(&mld6_lock, flags);
    return member;
}

static uint32_t mld6_max_response_ms(const uint16_t code)
{
    if (code < 32768)
        return code;
    const uint32_t mant = code & 0x0FFF;
    const uint32_t exp = (code >> 12) & 0x7;
    return (mant | 0x1000) << (exp + 3);
}

// TODO: MLDv1 compatibility mode (RFC 3810 8) for routers that only send v1 queries
void mld6_input(struct netbuf* nb, const size_t offset)
{
    struct ipv6_header ip;
    netbuf_copydata(nb, 0, sizeof(ip), &ip);
    const size_t len = nb->pkt_len - offset;

    if (len < 24 || ip.hop_limit != 1 || !ipv6_addr_is_linklocal(&ip.src)) {
        netbuf_free_chain(nb);
        return;
    }
    uint8_t q[24];
    netbuf_copydata(nb, offset, sizeof(q), q);
    struct netif* nif = nb->rcvif;
    netbuf_free_chain(nb);
    if (!nif)
        return;

    const uint16_t code = (uint16_t)(q[4] << 8 | q[5]);
    const uint32_t max_ms = len >= 28 ? mld6_max_response_ms(code) : code;
    struct in6_addr group;
    memcpy(&group, q + 8, sizeof(group));
    const bool general = ipv6_addr_is_unspecified(&group);
    const uint64_t now = ticks_get();

    uint64_t flags;
    spinlock_acquire_irqsave(&mld6_lock, &flags);
    for (struct dlist_node* n = dlist_first(&groups); n; n = dlist_next(n)) {
        const auto g = dlist_entry(n, struct mld6_group, link);
        if (g->nif != nif || !mld6_reportable(&g->addr) || (!general && !ipv6_addr_equal(&g->addr, &group)))
            continue;
        const uint64_t at = now + mld6_random_delay(max_ms);
        if (!g->report_at || at < g->report_at) {
            g->report_at = at;
            if (!g->reports_left)
                g->record_type = MLD6_MODE_IS_EXCLUDE;
        }
    }
    spinlock_release_irqrestore(&mld6_lock, flags);
}

void mld6_timer(void* arg)
{
    (void)arg;
    struct {
        struct netif* nif;
        struct in6_addr group;
        uint8_t type;
    } due[MLD6_MAX_SEND];
    size_t n_due = 0;
    const uint64_t now = ticks_get();

    uint64_t flags;
    spinlock_acquire_irqsave(&mld6_lock, &flags);
    for (struct dlist_node* n = dlist_first(&groups); n && n_due < MLD6_MAX_SEND; n = dlist_next(n)) {
        const auto g = dlist_entry(n, struct mld6_group, link);
        if (!g->report_at || g->report_at > now)
            continue;
        due[n_due].nif = g->nif;
        due[n_due].group = g->addr;
        due[n_due].type = g->record_type;
        n_due++;
        if (g->reports_left && --g->reports_left) {
            g->report_at = now + mld6_random_delay(MLD6_UNSOLICITED_MS);
        } else {
            g->report_at = 0;
            g->reports_left = 0;
        }
    }
    spinlock_release_irqrestore(&mld6_lock, flags);

    for (size_t i = 0; i < n_due; i++)
        mld6_send_report(due[i].nif, &due[i].group, due[i].type);
    callout_reset(&mld6_callout, ticks_from_ms(MLD6_TIMER_MS), mld6_timer, nullptr);
}
