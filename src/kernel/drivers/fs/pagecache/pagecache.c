#include "pagecache.h"

#include <hardware/memory/alloc.h>
#include <hardware/memory/mmu.h>
#include <hardware/memory/pmm.h>

uint8_t* pcache_find(const struct page_cache* pc, const size_t index)
{
    return index < pc->count ? pc->pages[index] : nullptr;
}

uint8_t* pcache_insert(struct page_cache* pc, const size_t index)
{
    if (index >= pc->count) {
        size_t count = pc->count ? pc->count * 2 : 16;
        if (count <= index)
            count = index + 1;
        uint8_t** pages = malloc(count * sizeof(uint8_t*));
        if (pages == nullptr)
            return nullptr;
        if (pc->count)
            memcpy(pages, pc->pages, pc->count * sizeof(uint8_t*));
        memset(pages + pc->count, 0, (count - pc->count) * sizeof(uint8_t*));
        free(pc->pages);
        pc->pages = pages;
        pc->count = count;
    }
    if (pc->pages[index] == nullptr) {
        pc->pages[index] = alloc_physical_page();
        if (pc->pages[index])
            memset(pc->pages[index], 0, PAGE_SIZE);
    }
    return pc->pages[index];
}

void pcache_remove(struct page_cache* pc, const size_t index)
{
    if (index < pc->count && pc->pages[index]) {
        free_physical_page(pc->pages[index]);
        pc->pages[index] = nullptr;
    }
}

void pcache_update(const struct page_cache* pc, const uint64_t pos, const void* src, const size_t len)
{
    for (size_t done = 0; done < len;) {
        const size_t in = (pos + done) % PAGE_SIZE;
        const size_t n = len - done < PAGE_SIZE - in ? len - done : PAGE_SIZE - in;
        uint8_t* page = pcache_find(pc, (pos + done) / PAGE_SIZE);
        if (page && src)
            memmove(page + in, (const uint8_t*)src + done, n);
        else if (page)
            memset(page + in, 0, n);
        done += n;
    }
}

void pcache_truncate(struct page_cache* pc, const uint64_t size)
{
    for (size_t i = (size + PAGE_SIZE - 1) / PAGE_SIZE; i < pc->count; i++)
        pcache_remove(pc, i);
    uint8_t* tail = size % PAGE_SIZE ? pcache_find(pc, size / PAGE_SIZE) : nullptr;
    if (tail)
        memset(tail + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
}

void pcache_destroy(struct page_cache* pc)
{
    for (size_t i = 0; i < pc->count; i++)
        pcache_remove(pc, i);
    free(pc->pages);
    pc->pages = nullptr;
    pc->count = 0;
}
