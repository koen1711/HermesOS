#include "pagecache.h"

#include <hardware/memory/alloc.h>
#include <hardware/memory/mmu.h>
#include <hardware/memory/pmm.h>
#include <utils/sync/spinlock.h>

static spinlock_t pcache_lock = { .name = "pcache" };
static struct dlist caches;

static uint8_t* lookup_locked(const struct page_cache* pc, const size_t index)
{
    return index < pc->count ? pc->pages[index] : nullptr;
}

static bool grow_locked(struct page_cache* pc, const size_t index)
{
    size_t count = pc->count ? pc->count * 2 : 16;
    if (count <= index)
        count = index + 1;
    uint8_t** pages = malloc(count * sizeof(uint8_t*));
    if (pages == nullptr)
        return false;
    if (pc->count)
        memcpy(pages, pc->pages, pc->count * sizeof(uint8_t*));
    memset(pages + pc->count, 0, (count - pc->count) * sizeof(uint8_t*));
    free(pc->pages);
    pc->pages = pages;
    pc->count = count;
    return true;
}

uint8_t* pcache_get(struct page_cache* pc, const size_t index)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&pcache_lock, &flags);
    uint8_t* page = lookup_locked(pc, index);
    if (page)
        pmm_share_page((void*)virt_to_phys(page));
    spinlock_release_irqrestore(&pcache_lock, flags);
    return page;
}

bool pcache_present(struct page_cache* pc, const size_t index)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&pcache_lock, &flags);
    const bool present = lookup_locked(pc, index) != nullptr;
    spinlock_release_irqrestore(&pcache_lock, flags);
    return present;
}

uint8_t* pcache_insert(struct page_cache* pc, const size_t index)
{
    uint8_t* fresh = alloc_kernel_page();
    if (fresh == nullptr)
        return nullptr;
    memset(fresh, 0, PAGE_SIZE);

    uint64_t flags;
    spinlock_acquire_irqsave(&pcache_lock, &flags);
    if (index >= pc->count && !grow_locked(pc, index)) {
        spinlock_release_irqrestore(&pcache_lock, flags);
        free_kernel_page(fresh);
        return nullptr;
    }
    if (!pc->listed) {
        dlist_insert_tail(&caches, &pc->link);
        pc->listed = true;
    }
    uint8_t* page = pc->pages[index];
    if (page == nullptr) {
        page = pc->pages[index] = fresh;
        fresh = nullptr;
    }
    pmm_share_page((void*)virt_to_phys(page));
    spinlock_release_irqrestore(&pcache_lock, flags);

    if (fresh)
        free_kernel_page(fresh);
    return page;
}

static void remove_locked(struct page_cache* pc, const size_t index)
{
    if (index < pc->count && pc->pages[index]) {
        free_kernel_page(pc->pages[index]);
        pc->pages[index] = nullptr;
    }
}

void pcache_remove(struct page_cache* pc, const size_t index)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&pcache_lock, &flags);
    remove_locked(pc, index);
    spinlock_release_irqrestore(&pcache_lock, flags);
}

void pcache_update(struct page_cache* pc, const uint64_t pos, const void* src, const size_t len)
{
    for (size_t done = 0; done < len;) {
        const size_t in = (pos + done) % PAGE_SIZE;
        const size_t n = len - done < PAGE_SIZE - in ? len - done : PAGE_SIZE - in;
        uint8_t* page = pcache_get(pc, (pos + done) / PAGE_SIZE);
        if (page && src)
            memmove(page + in, (const uint8_t*)src + done, n);
        else if (page)
            memset(page + in, 0, n);
        if (page)
            free_kernel_page(page);
        done += n;
    }
}

void pcache_truncate(struct page_cache* pc, const uint64_t size)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&pcache_lock, &flags);
    for (size_t i = (size + PAGE_SIZE - 1) / PAGE_SIZE; i < pc->count; i++)
        remove_locked(pc, i);
    spinlock_release_irqrestore(&pcache_lock, flags);

    uint8_t* tail = size % PAGE_SIZE ? pcache_get(pc, size / PAGE_SIZE) : nullptr;
    if (tail) {
        memset(tail + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
        free_kernel_page(tail);
    }
}

void pcache_destroy(struct page_cache* pc)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&pcache_lock, &flags);
    for (size_t i = 0; i < pc->count; i++)
        remove_locked(pc, i);
    if (pc->listed)
        dlist_remove(&caches, &pc->link);
    pc->listed = false;
    uint8_t** pages = pc->pages;
    pc->pages = nullptr;
    pc->count = 0;
    spinlock_release_irqrestore(&pcache_lock, flags);
    free(pages);
}

size_t pcache_reclaim(const size_t want)
{
    size_t freed = 0;
    uint64_t flags;
    spinlock_acquire_irqsave(&pcache_lock, &flags);
    for (size_t scanned = 0, total = caches.count; scanned < total && freed < want; scanned++) {
        struct dlist_node* node = dlist_first(&caches);
        struct page_cache* pc = dlist_entry(node, struct page_cache, link);
        dlist_remove(&caches, node);
        dlist_insert_tail(&caches, node);

        for (size_t i = 0; i < pc->count && freed < want; i++) {
            uint8_t* page = pc->pages[i];
            if (page == nullptr || pmm_page_shared((void*)virt_to_phys(page)))
                continue;
            pc->pages[i] = nullptr;
            free_kernel_page(page);
            freed++;
        }
    }
    spinlock_release_irqrestore(&pcache_lock, flags);
    return freed;
}
