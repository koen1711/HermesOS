#ifndef NET_IPV6_MLD6_H
#define NET_IPV6_MLD6_H

#include <os/stddef.h>
#include <drivers/net/ipv6/ipv6.h>

struct netif;
struct netbuf;

void mld6_initialize();
int mld6_join(struct netif* nif, const struct in6_addr* group);
int mld6_leave(struct netif* nif, const struct in6_addr* group);
bool mld6_is_member(struct netif* nif, const struct in6_addr* group);
void mld6_ifdetach(const struct netif* nif);
void mld6_input(struct netbuf* nb, size_t offset);
void mld6_timer(void* arg);

#endif //NET_IPV6_MLD6_H
