#ifndef NET_UDP_UDP_H
#define NET_UDP_UDP_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <drivers/net/socket/sockaddr.h>

#define UDP_HEADER_LEN 8

struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

struct netbuf;
struct socket;
struct inpcb;
struct inpcb_table;
struct protosw;

extern struct inpcb_table udp_pcbs;
extern struct protosw udp_protosw4;
extern struct protosw udp_protosw6;

void udp_initialize();

void udp_input4(struct netbuf* nb, size_t header_len);
void udp_input6(struct netbuf* nb, size_t offset);
int udp_output(struct inpcb* inp, struct netbuf* nb, const struct sockaddr* dst, socklen_t dst_len);

int udp_attach(struct socket* so, int proto);
int udp_bind(struct socket* so, const struct sockaddr* addr, socklen_t len);
int udp_connect(struct socket* so, const struct sockaddr* addr, socklen_t len);
int udp_disconnect(struct socket* so);
int udp_send(struct socket* so, struct netbuf* nb, const struct sockaddr* dst, socklen_t dst_len, int flags);
int udp_shutdown(struct socket* so, int how);
void udp_detach(struct socket* so);

#endif //NET_UDP_UDP_H
