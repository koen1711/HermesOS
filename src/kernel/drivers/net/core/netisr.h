#ifndef NET_CORE_NETISR_H
#define NET_CORE_NETISR_H

#include <os/stdint.h>

enum netisr_proto {
    NETISR_IP = 1,
    NETISR_IPV6,
    NETISR_ARP,
    NETISR_ETHER,
    NETISR_MAX,
};

struct netbuf;

typedef void (*netisr_handler_t)(struct netbuf* nb);

void netisr_initialize();
int netisr_register(enum netisr_proto proto, netisr_handler_t handler, uint32_t queue_len);
void netisr_unregister(enum netisr_proto proto);

int netisr_dispatch(enum netisr_proto proto, struct netbuf* nb);

int netisr_queue(enum netisr_proto proto, struct netbuf* nb);

// Runs the queued packets; re-entrant calls return at once (the outer call finishes the work)
void netisr_poll();
bool netisr_pending();
// netisr_poll, but only reports true when it ran here and handled packets
bool netisr_poll_pending();

#endif //NET_CORE_NETISR_H
