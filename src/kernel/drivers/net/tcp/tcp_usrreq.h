#ifndef NET_TCP_TCP_USRREQ_H
#define NET_TCP_TCP_USRREQ_H

#include <os/stdbool.h>
#include <drivers/net/socket/sockaddr.h>

struct socket;
struct netbuf;
struct protosw;

extern struct protosw tcp_protosw4;
extern struct protosw tcp_protosw6;

int tcp_attach(struct socket* so, int proto);
int tcp_bind(struct socket* so, const struct sockaddr* addr, socklen_t len);
int tcp_listen(struct socket* so, int backlog);
int tcp_connect(struct socket* so, const struct sockaddr* addr, socklen_t len);
int tcp_accept(struct socket* so, struct sockaddr* addr, socklen_t* len);
int tcp_send(struct socket* so, struct netbuf* nb, const struct sockaddr* dst, socklen_t dst_len, int flags);

int tcp_rcvd(struct socket* so, int flags);
int tcp_shutdown(struct socket* so, int how);
int tcp_disconnect(struct socket* so);
void tcp_abort(struct socket* so);
void tcp_detach(struct socket* so);
int tcp_ctloutput(struct socket* so, int level, int optname, void* optval, socklen_t* optlen, bool set);

#endif //NET_TCP_TCP_USRREQ_H
