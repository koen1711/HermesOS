#ifndef NET_UNIX_UNIX_H
#define NET_UNIX_UNIX_H

#include <drivers/net/socket/protosw.h>

struct socket;

extern struct protosw unix_stream_protosw;

int unix_socketpair(int type, int protocol, struct socket** a, struct socket** b);

#endif //NET_UNIX_UNIX_H
