#ifndef OS_DMA_H
#define OS_DMA_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <os/stdbool.h>

#define DMA_ADDR_MAX_24BIT  0x0000000000FFFFFFULL
#define DMA_ADDR_MAX_32BIT  0x00000000FFFFFFFFULL
#define DMA_ADDR_MAX        0xFFFFFFFFFFFFFFFFULL
#define DMA_SIZE_MAX_32BIT  0x00000000FFFFFFFFULL

typedef uint64_t dma_addr_t;
typedef uint64_t dma_size_t;

enum dma_flags {
    DMA_WAITOK = 0x0,
    DMA_NOWAIT = 0x1,
    DMA_ZERO = 0x2,
    DMA_COHERENT = 0x4,
    DMA_ALLOCNOW = 0x8
};

enum dma_sync_op {
    DMA_SYNC_PREREAD = 0x1,
    DMA_SYNC_POSTREAD = 0x2,
    DMA_SYNC_PREWRITE = 0x4,
    DMA_SYNC_POSTWRITE = 0x8
};

struct dma_segment {
    dma_addr_t addr;
    dma_size_t len;
};

struct dma_tag {
    struct dma_tag* parent;
    dma_size_t alignment;
    dma_addr_t boundary;
    dma_addr_t max_addr;
    dma_size_t max_size;
    int max_segments;
    dma_size_t max_segment_size;
    uint32_t flags;
    int ref_count;
    int map_count;
};

struct dma_map {
    struct dma_tag* tag;
    struct dma_segment* segments;
    int num_segments;
    void* buffer;
    dma_size_t buffer_len;
    void* bounce;
    dma_addr_t bounce_phys;
    dma_size_t bounce_len;
    bool bounced;
    bool loaded;
};

void dma_initialize();

int dma_tag_create(struct dma_tag* parent, dma_size_t alignment, dma_addr_t boundary, dma_addr_t max_addr,
                   dma_size_t max_size, int max_segments, dma_size_t max_segment_size, uint32_t flags,
                   struct dma_tag** out);
int dma_tag_destroy(struct dma_tag* tag);

int dma_map_create(struct dma_tag* tag, uint32_t flags, struct dma_map** out);
int dma_map_destroy(struct dma_map* map);

int dma_mem_alloc(struct dma_tag* tag, uint32_t flags, void** vaddr, struct dma_map** out);
void dma_mem_free(void* vaddr, struct dma_map* map);

int dma_map_load(struct dma_map* map, void* buffer, dma_size_t len, uint32_t flags);
void dma_map_unload(struct dma_map* map);
void dma_map_sync(struct dma_map* map, uint32_t op);

#endif //OS_DMA_H
