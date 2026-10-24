#ifndef NET_CORE_LOOPBACK_H
#define NET_CORE_LOOPBACK_H

#include <os/stdint.h>

#define LOOPBACK_MTU 16384

struct netif;
struct netbuf;

void loopback_initialize();
struct netif* loopback_netif();

int loopback_transmit(struct netif* nif, struct netbuf* nb);
int loopback_output(struct netif* nif, struct netbuf* nb, const void* dst, uint16_t family);

#endif //NET_CORE_LOOPBACK_H
