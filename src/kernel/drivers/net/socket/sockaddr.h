#ifndef NET_SOCKET_SOCKADDR_H
#define NET_SOCKET_SOCKADDR_H

#include <os/stdint.h>

typedef uint16_t sa_family_t;
typedef uint32_t socklen_t;

enum address_family
{
    AF_UNSPEC = 0,
    AF_INET = 2,
    AF_INET6 = 10
};

enum socket_type
{
    SOCK_STREAM = 1,
    SOCK_DGRAM = 2,
    SOCK_RAW = 3
};

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char ss_data[126];
};

#endif //NET_SOCKET_SOCKADDR_H
