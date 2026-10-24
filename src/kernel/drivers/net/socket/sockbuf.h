#ifndef NET_SOCKET_SOCKBUF_H
#define NET_SOCKET_SOCKBUF_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <os/stdbool.h>
#include <utils/sync/spinlock.h>
#include <drivers/net/socket/sockaddr.h>

#define SOCKBUF_DEFAULT_SIZE (64 * 1024)

enum sockbuf_flags
{
    SB_WAIT = 0x01,
    SB_NOINTR = 0x02,
    SB_CANTMORE = 0x04
};

#define NETBUF_SOCKADDR 0x0100

struct netbuf;

struct sockbuf {
    struct netbuf* head;
    struct netbuf* tail;
    size_t cc;
    size_t hiwat;
    size_t lowat;
    uint32_t flags;
    spinlock_t lock;
};

void sockbuf_init(struct sockbuf* sb, size_t hiwat);
void sockbuf_release(struct sockbuf* sb);

void sockbuf_append(struct sockbuf* sb, struct netbuf* nb);

bool sockbuf_append_addr(struct sockbuf* sb, const struct sockaddr* from, socklen_t from_len, struct netbuf* nb);

void sockbuf_drop(struct sockbuf* sb, size_t len);
void sockbuf_drop_record(struct sockbuf* sb);
size_t sockbuf_space(const struct sockbuf* sb);

void sockbuf_copy(const struct sockbuf* sb, size_t offset, size_t len, void* out);

int sockbuf_wait(struct sockbuf* sb);
void sockbuf_wakeup(struct sockbuf* sb);

#endif //NET_SOCKET_SOCKBUF_H
