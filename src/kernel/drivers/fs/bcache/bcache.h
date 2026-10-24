#ifndef BCACHE_H
#define BCACHE_H

#include <drivers/fs/fat/fat32.h>

struct block_device* bcache_attach(struct block_device* raw);

#endif //BCACHE_H
