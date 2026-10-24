#include "bcache.h"

#include <drivers/scheduler/process.h>
#include <drivers/timer/callout.h>
#include <hardware/memory/alloc.h>
#include <hardware/memory/pmm.h>
#include <hardware/smp/smp.h>
#include <utils/queue/queue.h>
#include <os/errno.h>

#define BCACHE_BLOCK        4096
#define BCACHE_MAX_BLOCKS   4096
#define BCACHE_HASH         1024
#define BCACHE_DIRTY_HIGH   (BCACHE_MAX_BLOCKS / 2)
#define BCACHE_FLUSH_SECS   5
#define BCACHE_DIRECT_MIN   (64 * 1024)
#define BCACHE_RA_MAX       32
#define BCACHE_RUN_MAX      256
#define WAIT_CHANNEL_BCACHE 0x3B000000
#define WAIT_CHANNEL_FLUSHER 0x3C000000

enum buf_flags {
    B_VALID = 0x1,
    B_DIRTY = 0x2,
    B_BUSY = 0x4
};

struct bcache_dev;

struct buf {
    struct dlist_node lru;
    struct buf* hash_next;
    struct bcache_dev* dev;
    uint64_t blkno;
    uint32_t flags;
    uint8_t* data;
};

struct bcache_dev {
    struct block_device bdev;
    struct block_device* raw;
    uint64_t next_seq;
    uint64_t last_flush;
};

static struct buf* hash_table[BCACHE_HASH];
static struct dlist lru;
static int nbufs;
static int ndirty;
static int lru_waiters;
static bool initialized;
static bool flusher_started;

static uint32_t hash_of(const struct bcache_dev* d, const uint64_t blkno)
{
    return (uint32_t)((((uintptr_t)d >> 4) ^ (blkno * 2654435761u)) % BCACHE_HASH);
}

static struct buf* lookup(const struct bcache_dev* d, const uint64_t blkno)
{
    for (struct buf* b = hash_table[hash_of(d, blkno)]; b; b = b->hash_next)
        if (b->dev == d && b->blkno == blkno)
            return b;
    return nullptr;
}

static void hash_insert(struct buf* b)
{
    struct buf** head = &hash_table[hash_of(b->dev, b->blkno)];
    b->hash_next = *head;
    *head = b;
}

static void hash_remove(const struct buf* b)
{
    for (struct buf** p = &hash_table[hash_of(b->dev, b->blkno)]; *p; p = &(*p)->hash_next) {
        if (*p == b) {
            *p = b->hash_next;
            return;
        }
    }
}

static int buf_wait_id(const struct buf* b)
{
    return WAIT_CHANNEL_BCACHE | (int)(((uintptr_t)b >> 6) & 0x00FFFFFF);
}

static void buf_release(struct buf* b)
{
    b->flags &= ~B_BUSY;
    dlist_insert_tail(&lru, &b->lru);
    process_wakeup(buf_wait_id(b));
    if (lru_waiters)
        process_wakeup(WAIT_CHANNEL_BCACHE);
}

static void buf_claim(struct buf* b)
{
    b->flags |= B_BUSY;
    dlist_remove(&lru, &b->lru);
}

static void mark_clean(struct buf* b)
{
    if (b->flags & B_DIRTY) {
        b->flags &= ~B_DIRTY;
        ndirty--;
    }
}

static uint32_t block_len(const struct bcache_dev* d, const uint64_t blkno)
{
    const uint64_t off = blkno * BCACHE_BLOCK;
    if (off >= d->bdev.part_length)
        return 0;
    const uint64_t left = d->bdev.part_length - off;
    return left < BCACHE_BLOCK ? (uint32_t)left : BCACHE_BLOCK;
}

static struct buf* buf_new(struct bcache_dev* d, const uint64_t blkno)
{
    if (nbufs >= BCACHE_MAX_BLOCKS)
        return nullptr;
    struct buf* b = calloc(1, sizeof(struct buf));
    if (!b)
        return nullptr;
    b->data = alloc_physical_page();
    if (!b->data) {
        free(b);
        return nullptr;
    }
    nbufs++;
    b->dev = d;
    b->blkno = blkno;
    b->flags = B_BUSY;
    hash_insert(b);
    return b;
}

static void buf_reassign(struct buf* b, struct bcache_dev* d, const uint64_t blkno)
{
    hash_remove(b);
    b->dev = d;
    b->blkno = blkno;
    b->flags = B_BUSY;
    hash_insert(b);
}

static int run_io(struct bcache_dev* d, struct buf** run, const int n, const bool write)
{
    const uint64_t off = run[0]->blkno * BCACHE_BLOCK;
    const uint32_t len = (uint32_t)(n - 1) * BCACHE_BLOCK + block_len(d, run[n - 1]->blkno);
    struct block_device* raw = d->raw;

    if (n == 1)
        return write ? raw->ops->write_bytes(raw, off, len, run[0]->data)
                     : raw->ops->read_bytes(raw, off, len, run[0]->data);

    uint8_t* staging = alloc_physical_pages((size_t)n);
    if (!staging) {
        for (int i = 0; i < n; i++) {
            const int err = run_io(d, &run[i], 1, write);
            if (err)
                return err;
        }
        return 0;
    }
    int err;
    if (write) {
        for (int i = 0; i < n; i++)
            memcpy(staging + (size_t)i * BCACHE_BLOCK, run[i]->data, BCACHE_BLOCK);
        err = raw->ops->write_bytes(raw, off, len, staging);
    } else {
        err = raw->ops->read_bytes(raw, off, len, staging);
        if (err == 0)
            for (int i = 0; i < n; i++)
                memcpy(run[i]->data, staging + (size_t)i * BCACHE_BLOCK, BCACHE_BLOCK);
    }
    free_physical_pages(staging, (size_t)n);
    return err;
}

static bool cluster_member(const struct buf* x)
{
    return x && !(x->flags & B_BUSY) && (x->flags & B_DIRTY);
}

static int writeback_cluster(struct buf* b)
{
    struct bcache_dev* d = b->dev;
    uint64_t first = b->blkno;
    while (b->blkno - first + 1 < BCACHE_RUN_MAX / 2 && first > 0 && cluster_member(lookup(d, first - 1)))
        first--;

    struct buf* run[BCACHE_RUN_MAX];
    int n = 0;
    for (uint64_t blk = first; n < BCACHE_RUN_MAX; blk++) {
        struct buf* x = blk == b->blkno ? b : lookup(d, blk);
        if (x != b && !cluster_member(x))
            break;
        if (x != b)
            buf_claim(x);
        run[n++] = x;
    }

    const int err = run_io(d, run, n, true);
    for (int i = 0; i < n; i++) {
        if (err == 0)
            mark_clean(run[i]);
        if (run[i] != b)
            buf_release(run[i]);
    }
    return err;
}

static struct buf* getblk(struct bcache_dev* d, const uint64_t blkno)
{
    for (;;) {
        struct buf* b = lookup(d, blkno);
        if (b) {
            if (b->flags & B_BUSY) {
                process_sleep(buf_wait_id(b));
                continue;
            }
            buf_claim(b);
            return b;
        }
        b = buf_new(d, blkno);
        if (b)
            return b;
        if (dlist_empty(&lru)) {
            lru_waiters++;
            process_sleep(WAIT_CHANNEL_BCACHE);
            lru_waiters--;
            continue;
        }
        struct buf* victim = dlist_entry(dlist_first(&lru), struct buf, lru);
        buf_claim(victim);
        if (victim->flags & B_DIRTY) {
            writeback_cluster(victim);
            buf_release(victim);
            continue;
        }
        buf_reassign(victim, d, blkno);
        return victim;
    }
}

static struct buf* getblk_nowait(struct bcache_dev* d, const uint64_t blkno)
{
    struct buf* b = buf_new(d, blkno);
    if (b)
        return b;
    for (struct dlist_node* n = dlist_first(&lru); n; n = dlist_next(n)) {
        b = dlist_entry(n, struct buf, lru);
        if (!(b->flags & B_DIRTY)) {
            buf_claim(b);
            buf_reassign(b, d, blkno);
            return b;
        }
    }
    return nullptr;
}

static struct buf* bread(struct bcache_dev* d, const uint64_t blkno)
{
    struct buf* b = getblk(d, blkno);
    if (b->flags & B_VALID)
        return b;

    struct buf* run[BCACHE_RA_MAX];
    run[0] = b;
    int n = 1;
    const int want = blkno == d->next_seq ? BCACHE_RA_MAX : 1;
    while (n < want && block_len(d, blkno + n) && !lookup(d, blkno + n)) {
        struct buf* x = getblk_nowait(d, blkno + n);
        if (!x)
            break;
        run[n++] = x;
    }

    const int err = run_io(d, run, n, false);
    for (int i = 0; i < n; i++) {
        const uint32_t valid = block_len(d, run[i]->blkno);
        if (err == 0) {
            if (valid < BCACHE_BLOCK)
                memset(run[i]->data + valid, 0, BCACHE_BLOCK - valid);
            run[i]->flags |= B_VALID;
        }
        if (i > 0)
            buf_release(run[i]);
    }
    d->next_seq = blkno + (uint64_t)n;
    if (err) {
        buf_release(b);
        return nullptr;
    }
    return b;
}

static int flush_dirty(struct bcache_dev* d)
{
    int err = 0;
    for (;;) {
        struct buf* idle = nullptr;
        struct buf* busy = nullptr;
        for (int h = 0; h < BCACHE_HASH; h++) {
            for (struct buf* b = hash_table[h]; b; b = b->hash_next) {
                if (!(b->flags & B_DIRTY) || (d && b->dev != d))
                    continue;
                if (b->flags & B_BUSY)
                    busy = b;
                else if (!idle || b->dev < idle->dev || (b->dev == idle->dev && b->blkno < idle->blkno))
                    idle = b;
            }
        }
        if (idle) {
            buf_claim(idle);
            const int e = writeback_cluster(idle);
            buf_release(idle);
            if (e) {
                err = e;
                break;
            }
            continue;
        }
        if (!busy)
            break;
        process_sleep(buf_wait_id(busy));
    }
    if (d)
        d->last_flush = ticks_get();
    return err;
}

static void flusher(void* arg)
{
    (void)arg;
    for (;;) {
        process_sleep_timeout(WAIT_CHANNEL_FLUSHER, (uint64_t)BCACHE_FLUSH_SECS * ticks_hz());
        const bool locked = kernel_lock_enter();
        if (ndirty)
            flush_dirty(nullptr);
        if (locked)
            kernel_lock_exit();
    }
}

static void maybe_flush(struct bcache_dev* d)
{
    if (ndirty > BCACHE_DIRTY_HIGH || (ndirty && ticks_get() - d->last_flush > (uint64_t)BCACHE_FLUSH_SECS * ticks_hz()))
        flush_dirty(d);
}

static int direct_read(struct bcache_dev* d, const uint64_t off, const uint32_t len, uint8_t* out)
{
    const int err = d->raw->ops->read_bytes(d->raw, off, len, out);
    if (err)
        return err;
    for (uint64_t blk = off / BCACHE_BLOCK; blk < (off + len) / BCACHE_BLOCK; blk++) {
        const struct buf* b = lookup(d, blk);
        if (b && (b->flags & B_VALID))
            memcpy(out + (blk * BCACHE_BLOCK - off), b->data, BCACHE_BLOCK);
    }
    return 0;
}

static int bc_read(struct block_device* bdev, uint64_t off, uint32_t len, void* buf)
{
    struct bcache_dev* d = bdev->private;
    maybe_flush(d);
    uint8_t* out = buf;
    while (len) {
        const uint32_t in = (uint32_t)(off % BCACHE_BLOCK);
        if (in == 0 && len >= BCACHE_DIRECT_MIN) {
            const uint32_t span = len & ~(uint32_t)(BCACHE_BLOCK - 1);
            const int err = direct_read(d, off, span, out);
            if (err)
                return err;
            off += span;
            out += span;
            len -= span;
            continue;
        }
        const uint32_t n = BCACHE_BLOCK - in < len ? BCACHE_BLOCK - in : len;
        struct buf* b = bread(d, off / BCACHE_BLOCK);
        if (!b)
            return -EIO;
        memcpy(out, b->data + in, n);
        buf_release(b);
        off += n;
        out += n;
        len -= n;
    }
    return 0;
}

static int bc_write(struct block_device* bdev, uint64_t off, uint32_t len, const void* buf)
{
    struct bcache_dev* d = bdev->private;
    const uint8_t* src = buf;
    while (len) {
        const uint64_t blkno = off / BCACHE_BLOCK;
        const uint32_t in = (uint32_t)(off % BCACHE_BLOCK);
        const uint32_t n = BCACHE_BLOCK - in < len ? BCACHE_BLOCK - in : len;
        const bool whole = in == 0 && n >= block_len(d, blkno);
        struct buf* b = whole ? getblk(d, blkno) : bread(d, blkno);
        if (!b)
            return -EIO;
        memcpy(b->data + in, src, n);
        b->flags |= B_VALID;
        if (!(b->flags & B_DIRTY)) {
            b->flags |= B_DIRTY;
            ndirty++;
        }
        buf_release(b);
        off += n;
        src += n;
        len -= n;
    }
    if (!flusher_started)
        flusher_started = process_create_kernel(flusher, nullptr) != nullptr;
    maybe_flush(d);
    return 0;
}

static int bc_flush(struct block_device* bdev)
{
    struct bcache_dev* d = bdev->private;
    const int err = flush_dirty(d);
    if (err)
        return err;
    return d->raw->ops->flush ? d->raw->ops->flush(d->raw) : 0;
}

static void bc_release(struct block_device* bdev)
{
    struct bcache_dev* d = bdev->private;
    bc_flush(bdev);
    for (int h = 0; h < BCACHE_HASH; h++) {
        struct buf** p = &hash_table[h];
        while (*p) {
            struct buf* b = *p;
            if (b->dev != d) {
                p = &b->hash_next;
                continue;
            }
            if (b->flags & B_BUSY) {
                process_sleep(buf_wait_id(b));
                p = &hash_table[h];
                continue;
            }
            if (b->flags & B_DIRTY)
                ndirty--;
            *p = b->hash_next;
            dlist_remove(&lru, &b->lru);
            free_physical_page(b->data);
            free(b);
            nbufs--;
            p = &hash_table[h];
        }
    }
    free(d->raw);
    free(d);
}

static const struct block_device_ops bcache_ops = {
    .read_bytes  = bc_read,
    .write_bytes = bc_write,
    .flush       = bc_flush,
    .release     = bc_release,
};

struct block_device* bcache_attach(struct block_device* raw)
{
    if (!initialized) {
        dlist_init(&lru);
        initialized = true;
    }
    struct bcache_dev* d = calloc(1, sizeof(struct bcache_dev));
    if (!d)
        return nullptr;
    d->raw = raw;
    d->next_seq = ~0ULL;
    d->last_flush = ticks_get();
    d->bdev.unique_id = raw->unique_id;
    d->bdev.part_offset = raw->part_offset;
    d->bdev.part_length = raw->part_length;
    d->bdev.ops = &bcache_ops;
    d->bdev.private = d;
    return &d->bdev;
}
