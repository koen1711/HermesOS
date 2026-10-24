#include <drivers/net/socket/protosw.h>

#include <drivers/net/ipv4/ipv4.h>
#include <drivers/net/udp/udp.h>
#include <drivers/net/tcp/tcp_usrreq.h>
#include <drivers/net/unix/unix.h>
#include <os/errno.h>

#define PROTOSW_MAX_DOMAINS 4
#define PROTOSW_MAX_PER_DOMAIN 8

static struct domain* domains[PROTOSW_MAX_DOMAINS];
static uint32_t domain_count;

static struct protosw inet_protos[PROTOSW_MAX_PER_DOMAIN];
static struct protosw inet6_protos[PROTOSW_MAX_PER_DOMAIN];
static struct protosw unix_protos[PROTOSW_MAX_PER_DOMAIN];
static struct domain inet_domain = { .family = AF_INET, .name = "inet", .protosw = inet_protos };
static struct domain inet6_domain = { .family = AF_INET6, .name = "inet6", .protosw = inet6_protos };
static struct domain unix_domain = { .family = AF_UNIX, .name = "unix", .protosw = unix_protos };

void protosw_initialize()
{
    domain_count = 0;
    inet_domain.protosw_count = 0;
    inet6_domain.protosw_count = 0;
    unix_domain.protosw_count = 0;
    domain_register(&inet_domain);
    domain_register(&inet6_domain);
    domain_register(&unix_domain);

    protosw_register(&inet_domain, &tcp_protosw4);
    protosw_register(&inet_domain, &udp_protosw4);
    protosw_register(&inet6_domain, &tcp_protosw6);
    protosw_register(&inet6_domain, &udp_protosw6);
    protosw_register(&unix_domain, &unix_stream_protosw);
}

int domain_register(struct domain* dom)
{
    if (domain_count >= PROTOSW_MAX_DOMAINS)
        return -ENOSPC;
    for (uint32_t i = 0; i < domain_count; i++)
        if (domains[i]->family == dom->family)
            return -EEXIST;
    domains[domain_count++] = dom;
    return 0;
}

int protosw_register(struct domain* dom, struct protosw* pr)
{
    if (dom->protosw_count >= PROTOSW_MAX_PER_DOMAIN)
        return -ENOSPC;
    dom->protosw[dom->protosw_count++] = *pr;
    return 0;
}

struct protosw* protosw_find(const int domain, const int type, const int protocol)
{
    for (uint32_t d = 0; d < domain_count; d++) {
        const struct domain* dom = domains[d];
        if (dom->family != domain)
            continue;
        for (uint32_t i = 0; i < dom->protosw_count; i++) {
            struct protosw* pr = &dom->protosw[i];
            if (pr->type == type && (protocol == 0 || pr->protocol == protocol))
                return pr;
        }
    }
    return nullptr;
}
