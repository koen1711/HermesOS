#ifndef FS_PAGECACHE_H
#define FS_PAGECACHE_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <os/stdbool.h>
#include <utils/queue/queue.h>

struct page_cache {
    uint8_t** pages;
    size_t count;
    struct dlist_node link;
    bool listed;
};

uint8_t* pcache_get(struct page_cache* pc, size_t index);
bool pcache_present(struct page_cache* pc, size_t index);
uint8_t* pcache_insert(struct page_cache* pc, size_t index);
void pcache_remove(struct page_cache* pc, size_t index);
void pcache_update(struct page_cache* pc, uint64_t pos, const void* src, size_t len);
void pcache_truncate(struct page_cache* pc, uint64_t size);
void pcache_destroy(struct page_cache* pc);
size_t pcache_reclaim(size_t want);

#endif //FS_PAGECACHE_H
