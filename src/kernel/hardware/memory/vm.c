#include "vm.h"

#include "alloc.h"
#include "pmm.h"
#include <hardware/smp/smp.h>
#include <os/errno.h>

#define PTE_FRAME 0x000FFFFFFFFFF000ULL
#define PTE_DIRTY 0x40ULL
#define PTE_COW   0x200ULL

struct vm_flush {
    struct vm_flush* next;
    const struct vm_file_ops* ops;
    void* file;
    uint64_t offset;
    uint64_t frame;
};

static struct vm_area* area_of(const struct dlist_node* node)
{
    return node ? dlist_entry(node, struct vm_area, link) : nullptr;
}

static uint64_t pte_flags(const uint32_t prot)
{
    if (prot == VM_NONE)
        return MMU_USER;
    return MMU_PRESENT | MMU_USER | (prot & VM_WRITE ? MMU_WRITABLE : 0);
}

static uint64_t pte_with_prot(const uint64_t pte, const uint32_t prot)
{
    const uint64_t flags = pte_flags(prot) & (pte & PTE_COW ? ~(uint64_t)MMU_WRITABLE : ~0ULL);
    return (pte & (PTE_FRAME | PTE_DIRTY | PTE_COW)) | flags;
}

static bool user_range(const uint64_t start, const uint64_t end)
{
    return start >= USER_ADDR_MIN && end <= USER_ADDR_LIMIT && start < end;
}

static void invalidate(const uint64_t va)
{
    asm volatile("invlpg (%0)" : : "r"(va) : "memory");
}

static struct vm_area* area_new(const uint64_t start, const uint64_t end, const uint32_t prot, const uint32_t flags)
{
    struct vm_area* a = malloc(sizeof(struct vm_area));
    if (!a)
        return nullptr;
    memset(a, 0, sizeof(*a));
    a->start = start;
    a->end = end;
    a->prot = prot;
    a->flags = flags;
    return a;
}

static void area_insert(struct vm_space* vm, struct vm_area* a)
{
    for (struct dlist_node* n = dlist_first(&vm->areas); n; n = dlist_next(n)) {
        if (area_of(n)->start > a->start) {
            dlist_insert_before(&vm->areas, n, &a->link);
            return;
        }
    }
    dlist_insert_tail(&vm->areas, &a->link);
}

static void area_set_file(struct vm_area* a, const struct vm_area* src, const uint64_t offset)
{
    a->fops = src->fops;
    a->file = src->file;
    a->offset = offset;
    if (a->file)
        a->fops->acquire(a->file);
}

static void queue(struct vm_space* vm, const struct vm_area* a, const uint64_t offset, const uint64_t frame)
{
    struct vm_flush* w = malloc(sizeof(struct vm_flush));
    if (!w) {
        if (frame)
            free_physical_page((void*)frame);
        return;
    }
    w->next = nullptr;
    w->ops = a->fops;
    w->file = a->file;
    w->offset = offset;
    w->frame = frame;
    if (vm->pending_tail)
        vm->pending_tail->next = w;
    else
        vm->pending = w;
    vm->pending_tail = w;
}

static void queue_dirty(struct vm_space* vm, const struct vm_area* a, const uint64_t start, const uint64_t end,
                        const bool clear)
{
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t* pte = mmu_find_pte(vm->page_map, va);
        if (!pte || !(*pte & PTE_FRAME) || !(*pte & PTE_DIRTY))
            continue;
        pmm_share_page((void*)(*pte & PTE_FRAME));
        queue(vm, a, a->offset + (va - a->start), *pte & PTE_FRAME);
        if (clear) {
            *pte &= ~PTE_DIRTY;
            invalidate(va);
        }
    }
}

static void run_flush(struct vm_flush* w)
{
    while (w) {
        struct vm_flush* next = w->next;
        if (w->frame) {
            w->ops->write(w->file, w->offset, phys_to_virt(w->frame), PAGE_SIZE);
            free_physical_page((void*)w->frame);
        } else {
            w->ops->release(w->file);
        }
        free(w);
        w = next;
    }
}

static void drain(struct vm_space* vm)
{
    spinlock_acquire(&vm->lock);
    struct vm_flush* w = vm->pending;
    vm->pending = vm->pending_tail = nullptr;
    spinlock_release(&vm->lock);
    if (!w)
        return;
    const bool locked = kernel_lock_enter();
    run_flush(w);
    if (locked)
        kernel_lock_exit();
}

static int area_split(struct vm_space* vm, struct vm_area* a, const uint64_t at)
{
    struct vm_area* b = area_new(at, a->end, a->prot, a->flags);
    if (!b)
        return -ENOMEM;
    area_set_file(b, a, a->offset + (at - a->start));
    a->end = at;
    dlist_insert_after(&vm->areas, &a->link, &b->link);
    return 0;
}

static int range_isolate(struct vm_space* vm, const uint64_t start, const uint64_t end)
{
    for (struct dlist_node* n = dlist_first(&vm->areas); n; n = dlist_next(n)) {
        struct vm_area* a = area_of(n);
        if (a->start >= end)
            break;
        int err = 0;
        if (a->start < start && start < a->end)
            err = area_split(vm, a, start);
        else if (a->start < end && end < a->end)
            err = area_split(vm, a, end);
        if (err)
            return err;
    }
    return 0;
}

static void free_frames(struct vm_space* vm, const uint64_t start, const uint64_t end)
{
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t* pte = mmu_find_pte(vm->page_map, va);
        if (!pte || !(*pte & MMU_USER) || !(*pte & PTE_FRAME))
            continue;
        free_physical_page((void*)(*pte & PTE_FRAME));
        *pte = 0;
        invalidate(va);
    }
}

static uint64_t* frame_for(struct vm_space* vm, const uint64_t va, const uint32_t prot)
{
    uint64_t* pte = mmu_find_pte(vm->page_map, va);
    if (pte && (*pte & PTE_FRAME))
        return pte;

    void* page = alloc_physical_page();
    if (!page)
        return nullptr;
    memset(phys_to_virt((uint64_t)page), 0, PAGE_SIZE);
    if (map_page_in(vm->page_map, va, (uint64_t)page, pte_flags(prot)) < 0) {
        free_physical_page(page);
        return nullptr;
    }
    return mmu_find_pte(vm->page_map, va);
}

static void flush_others(const struct vm_space* vm)
{
    if (vm->ref_count > 1)
        smp_tlb_shootdown();
}

static bool cow_break(const struct vm_space* vm, const uint64_t va, uint64_t* pte)
{
    const uint64_t old = *pte & PTE_FRAME;
    if (!pmm_page_shared((void*)old)) {
        *pte &= ~PTE_COW;
        return true;
    }
    void* page = alloc_physical_page();
    if (!page)
        return false;
    memcpy(phys_to_virt((uint64_t)page), phys_to_virt(old), PAGE_SIZE);
    *pte = (uint64_t)page | (*pte & ~(PTE_FRAME | PTE_COW));
    invalidate(va);
    flush_others(vm);
    free_physical_page((void*)old);
    return true;
}

struct vm_space* vm_create()
{
    struct vm_space* vm = malloc(sizeof(struct vm_space));
    if (!vm)
        return nullptr;
    memset(vm, 0, sizeof(*vm));
    vm->page_map = setup_kvm();
    if (!vm->page_map) {
        free(vm);
        return nullptr;
    }
    spinlock_init(&vm->lock, "vm");
    vm->ref_count = 1;
    vm->mmap_base = USER_MMAP_BASE;
    dlist_init(&vm->areas);
    return vm;
}

static struct vm_space* clone_locked(struct vm_space* src)
{
    struct vm_space* vm = vm_create();
    if (!vm)
        return nullptr;
    vm->brk_start = src->brk_start;
    vm->brk = src->brk;
    vm->mmap_base = src->mmap_base;

    for (const struct dlist_node* n = dlist_first(&src->areas); n; n = dlist_next(n)) {
        const struct vm_area* a = area_of(n);
        struct vm_area* copy = area_new(a->start, a->end, a->prot, a->flags);
        if (!copy) {
            vm_release(vm);
            return nullptr;
        }
        area_set_file(copy, a, a->offset);
        dlist_insert_tail(&vm->areas, &copy->link);

        for (uint64_t va = a->start; va < a->end; va += PAGE_SIZE) {
            uint64_t* pte = mmu_find_pte(src->page_map, va);
            if (!pte || !(*pte & PTE_FRAME))
                continue;
            if (!(a->flags & VMA_SHARED) && !(*pte & PTE_COW)) {
                *pte = (*pte & ~(uint64_t)MMU_WRITABLE) | PTE_COW;
                invalidate(va);
            }
            if (map_page_in(vm->page_map, va, *pte & PTE_FRAME, *pte & ~PTE_FRAME) < 0) {
                flush_others(src);
                vm_release(vm);
                return nullptr;
            }
            pmm_share_page((void*)(*pte & PTE_FRAME));
        }
    }
    flush_others(src);
    return vm;
}

struct vm_space* vm_clone(struct vm_space* src)
{
    spinlock_acquire(&src->lock);
    struct vm_space* vm = clone_locked(src);
    spinlock_release(&src->lock);
    return vm;
}

void vm_acquire(struct vm_space* vm)
{
    vm->ref_count++;
}

void vm_release(struct vm_space* vm)
{
    if (!vm || --vm->ref_count > 0)
        return;
    while (!dlist_empty(&vm->areas)) {
        struct dlist_node* n = dlist_first(&vm->areas);
        const struct vm_area* a = area_of(n);
        if (a->file) {
            queue_dirty(vm, a, a->start, a->end, false);
            queue(vm, a, 0, 0);
        }
        dlist_remove(&vm->areas, n);
        free(area_of(n));
    }
    run_flush(vm->pending);
    free_vm(vm->page_map);
    free(vm);
}

struct vm_area* vm_find(const struct vm_space* vm, const uint64_t addr)
{
    for (struct dlist_node* n = dlist_first(&vm->areas); n; n = dlist_next(n)) {
        struct vm_area* a = area_of(n);
        if (addr < a->start)
            return nullptr;
        if (addr < a->end)
            return a;
    }
    return nullptr;
}

bool vm_range_available(struct vm_space* vm, const uint64_t start, const uint64_t end)
{
    spinlock_acquire(&vm->lock);
    const bool available = vm_range_free(vm, start, end);
    spinlock_release(&vm->lock);
    return available;
}

bool vm_range_free(const struct vm_space* vm, const uint64_t start, const uint64_t end)
{
    for (struct dlist_node* n = dlist_first(&vm->areas); n; n = dlist_next(n)) {
        const struct vm_area* a = area_of(n);
        if (a->start >= end)
            break;
        if (start < a->end)
            return false;
    }
    return true;
}

static uint64_t find_gap(const struct vm_space* vm, const size_t len)
{
    uint64_t candidate = vm->mmap_base;
    for (struct dlist_node* n = dlist_first(&vm->areas); n; n = dlist_next(n)) {
        const struct vm_area* a = area_of(n);
        if (a->end <= candidate)
            continue;
        if (a->start >= candidate + len)
            break;
        candidate = a->end;
    }
    return candidate + len <= USER_STACK_TOP - USER_STACK_LIMIT ? candidate : 0;
}

static int unmap_locked(struct vm_space* vm, uint64_t addr, size_t len);

static int map_locked(struct vm_space* vm, const uint64_t addr, size_t len, const uint32_t prot, const uint32_t flags,
                      const bool fixed, uint64_t* out)
{
    len = PAGE_ALIGN_UP(len);
    if (len == 0 || (addr & ~PAGE_MASK))
        return -EINVAL;

    uint64_t start = addr;
    if (fixed) {
        if (!user_range(start, start + len))
            return -EINVAL;
        const int err = unmap_locked(vm, start, len);
        if (err)
            return err;
    } else if (!addr || !user_range(addr, addr + len) || !vm_range_free(vm, addr, addr + len)) {
        start = find_gap(vm, len);
        if (!start)
            return -ENOMEM;
    }

    struct vm_area* a = area_new(start, start + len, prot, flags);
    if (!a)
        return -ENOMEM;
    area_insert(vm, a);
    if (out)
        *out = start;
    return 0;
}

static int unmap_locked(struct vm_space* vm, const uint64_t addr, const size_t len)
{
    const uint64_t end = PAGE_ALIGN_UP(addr + len);
    if ((addr & ~PAGE_MASK) || len == 0 || end < addr)
        return -EINVAL;
    const int err = range_isolate(vm, addr, end);
    if (err)
        return err;

    struct dlist_node* n = dlist_first(&vm->areas);
    while (n) {
        struct dlist_node* next = dlist_next(n);
        struct vm_area* a = area_of(n);
        if (a->start >= end)
            break;
        if (a->start >= addr) {
            if (a->file) {
                queue_dirty(vm, a, a->start, a->end, false);
                queue(vm, a, 0, 0);
            }
            free_frames(vm, a->start, a->end);
            dlist_remove(&vm->areas, n);
            free(a);
        }
        n = next;
    }
    smp_tlb_shootdown();
    return 0;
}

static int protect_locked(struct vm_space* vm, const uint64_t addr, const size_t len, const uint32_t prot)
{
    const uint64_t end = PAGE_ALIGN_UP(addr + len);
    if ((addr & ~PAGE_MASK) || end < addr)
        return -EINVAL;
    if (len == 0)
        return 0;
    for (uint64_t va = addr; va < end;) {
        const struct vm_area* a = vm_find(vm, va);
        if (!a)
            return -ENOMEM;
        if (prot & VM_WRITE && a->flags & VMA_NO_WRITE)
            return -EACCES;
        va = a->end;
    }
    const int err = range_isolate(vm, addr, end);
    if (err)
        return err;

    for (struct dlist_node* n = dlist_first(&vm->areas); n; n = dlist_next(n)) {
        struct vm_area* a = area_of(n);
        if (a->start >= end)
            break;
        if (a->start < addr)
            continue;
        a->prot = prot;
        for (uint64_t va = a->start; va < a->end; va += PAGE_SIZE) {
            uint64_t* pte = mmu_find_pte(vm->page_map, va);
            if (!pte || !(*pte & PTE_FRAME))
                continue;
            *pte = pte_with_prot(*pte, prot);
            invalidate(va);
        }
    }
    smp_tlb_shootdown();
    return 0;
}

static int discard_locked(struct vm_space* vm, const uint64_t addr, const size_t len)
{
    const uint64_t end = PAGE_ALIGN_UP(addr + len);
    if ((addr & ~PAGE_MASK) || end < addr || !user_range(addr, end))
        return -EINVAL;

    for (uint64_t va = addr; va < end;) {
        const struct vm_area* a = vm_find(vm, va);
        if (!a)
            return -ENOMEM;
        const uint64_t stop = a->end < end ? a->end : end;

        if (!(a->flags & VMA_SHARED))
            free_frames(vm, va, stop);
        va = stop;
    }
    smp_tlb_shootdown();
    return 0;
}

static int sync_locked(struct vm_space* vm, const uint64_t addr, const size_t len)
{
    const uint64_t end = PAGE_ALIGN_UP(addr + len);
    if ((addr & ~PAGE_MASK) || end < addr)
        return -EINVAL;
    for (uint64_t va = addr; va < end;) {
        const struct vm_area* a = vm_find(vm, va);
        if (!a)
            return -ENOMEM;
        const uint64_t stop = a->end < end ? a->end : end;
        if (a->file)
            queue_dirty(vm, a, va, stop, true);
        va = stop;
    }
    smp_tlb_shootdown();
    return 0;
}

static int insert_frame_locked(struct vm_space* vm, const uint64_t va, const uint64_t frame)
{
    const struct vm_area* a = vm_find(vm, va);
    if (!a || (va & ~PAGE_MASK))
        return -EFAULT;
    const uint64_t* pte = mmu_find_pte(vm->page_map, va);
    const uint64_t old = pte ? *pte & PTE_FRAME : 0;
    if (map_page_in(vm->page_map, va, frame, pte_flags(a->prot)) < 0)
        return -ENOMEM;
    if (old)
        free_physical_page((void*)old);
    invalidate(va);
    return 0;
}

static uint64_t brk_locked(struct vm_space* vm, const uint64_t new_brk)
{
    if (new_brk < vm->brk_start)
        return vm->brk;
    const uint64_t old_end = PAGE_ALIGN_UP(vm->brk);
    const uint64_t new_end = PAGE_ALIGN_UP(new_brk);

    if (new_end > old_end) {
        if (!user_range(old_end, new_end) || !vm_range_free(vm, old_end, new_end))
            return vm->brk;
        struct vm_area* heap = old_end > vm->brk_start ? vm_find(vm, old_end - 1) : nullptr;
        if (heap && (heap->flags & VMA_HEAP) && heap->end == old_end) {
            heap->end = new_end;
        } else if (map_locked(vm, old_end, new_end - old_end, VM_READ | VM_WRITE, VMA_HEAP, true, nullptr) < 0) {
            return vm->brk;
        }
    } else if (new_end < old_end) {
        unmap_locked(vm, new_end, old_end - new_end);
    }
    vm->brk = new_brk;
    return vm->brk;
}

static int move_locked(struct vm_space* vm, const uint64_t old_addr, const size_t len, const uint64_t new_addr,
                       const size_t new_len)
{
    const int err = range_isolate(vm, old_addr, old_addr + len);
    if (err)
        return err;
    struct vm_area* src = vm_find(vm, old_addr);
    struct vm_area* dst = area_new(new_addr, new_addr + new_len, src->prot, src->flags);
    if (!dst)
        return -ENOMEM;
    dst->fops = src->fops;
    dst->file = src->file;
    dst->offset = src->offset;

    uint64_t off = 0;
    for (; off < len; off += PAGE_SIZE) {
        uint64_t* pte = mmu_find_pte(vm->page_map, old_addr + off);
        if (!pte || !(*pte & PTE_FRAME))
            continue;
        if (map_page_in(vm->page_map, new_addr + off, *pte & PTE_FRAME, *pte & ~PTE_FRAME) < 0)
            break;
        *pte = 0;
        invalidate(old_addr + off);
    }
    if (off < len) {
        for (uint64_t back = 0; back < off; back += PAGE_SIZE) {
            uint64_t* pte = mmu_find_pte(vm->page_map, new_addr + back);
            if (!pte || !(*pte & PTE_FRAME))
                continue;
            *mmu_find_pte(vm->page_map, old_addr + back) = *pte;
            *pte = 0;
            invalidate(new_addr + back);
        }
        free(dst);
        smp_tlb_shootdown();
        return -ENOMEM;
    }

    dlist_remove(&vm->areas, &src->link);
    free(src);
    area_insert(vm, dst);
    smp_tlb_shootdown();
    return 0;
}

static int remap_locked(struct vm_space* vm, const uint64_t old_addr, size_t old_len, size_t new_len, const int flags,
                        uint64_t* out)
{
    old_len = PAGE_ALIGN_UP(old_len);
    new_len = PAGE_ALIGN_UP(new_len);
    const uint64_t old_end = old_addr + old_len;
    if ((old_addr & ~PAGE_MASK) || old_len == 0 || new_len == 0 || old_end < old_addr)
        return -EINVAL;
    struct vm_area* a = vm_find(vm, old_addr);
    if (!a || old_end > a->end)
        return -EFAULT;

    if (new_len <= old_len) {
        if (new_len < old_len) {
            const int err = unmap_locked(vm, old_addr + new_len, old_len - new_len);
            if (err)
                return err;
        }
        *out = old_addr;
        return 0;
    }

    const uint64_t new_end = old_addr + new_len;
    if (old_end == a->end && user_range(old_addr, new_end) && vm_range_free(vm, old_end, new_end)) {
        a->end = new_end;
        *out = old_addr;
        return 0;
    }
    if (!(flags & MREMAP_MAYMOVE))
        return -ENOMEM;

    const uint64_t new_addr = find_gap(vm, new_len);
    if (!new_addr)
        return -ENOMEM;
    const int err = move_locked(vm, old_addr, old_len, new_addr, new_len);
    if (err == 0)
        *out = new_addr;
    return err;
}

static int remap_fixed_locked(struct vm_space* vm, const uint64_t old_addr, size_t old_len, size_t new_len,
                              const int flags, const uint64_t new_addr, uint64_t* out)
{
    old_len = PAGE_ALIGN_UP(old_len);
    new_len = PAGE_ALIGN_UP(new_len);
    const uint64_t old_end = old_addr + old_len;
    const uint64_t new_end = new_addr + new_len;
    if ((old_addr & ~PAGE_MASK) || (new_addr & ~PAGE_MASK) || old_len == 0 || new_len == 0 || old_end < old_addr
        || !(flags & MREMAP_MAYMOVE) || !user_range(new_addr, new_end))
        return -EINVAL;
    if (new_addr < old_end && old_addr < new_end)
        return -EINVAL;
    const struct vm_area* a = vm_find(vm, old_addr);
    if (!a || old_end > a->end)
        return -EFAULT;

    int err = unmap_locked(vm, new_addr, new_len);
    if (err)
        return err;
    if (new_len < old_len) {
        err = unmap_locked(vm, old_addr + new_len, old_len - new_len);
        if (err)
            return err;
        old_len = new_len;
    }
    err = move_locked(vm, old_addr, old_len, new_addr, new_len);
    if (err == 0)
        *out = new_addr;
    return err;
}

static bool fault_locked(struct vm_space* vm, const uint64_t addr, const bool write)
{
    const struct vm_area* a = vm_find(vm, addr);
    if (!a || !(a->prot & (write ? VM_WRITE : VM_READ)))
        return false;

    const uint64_t va = addr & PAGE_MASK;
    uint64_t* pte = mmu_find_pte(vm->page_map, va);
    if (pte && (*pte & MMU_PRESENT) && (!write || (*pte & MMU_WRITABLE))) {
        invalidate(va);
        return true;
    }
    pte = frame_for(vm, va, a->prot);
    if (!pte || (write && (*pte & PTE_COW) && !cow_break(vm, va, pte)))
        return false;
    *pte = pte_with_prot(*pte, a->prot);
    invalidate(va);
    return true;
}

static bool populate_locked(struct vm_space* vm, const uint64_t addr, const size_t len, const bool write)
{
    if (len == 0)
        return true;
    const uint64_t end = addr + len;
    if (end < addr || !user_range(addr, end))
        return false;

    const uint32_t need = write ? VM_WRITE : VM_READ;
    const struct vm_area* a = nullptr;
    for (uint64_t va = addr & PAGE_MASK; va < end; va += PAGE_SIZE) {
        if (!a || va >= a->end) {
            a = vm_find(vm, va);
            if (!a || !(a->prot & need))
                return false;
        }
        const uint64_t* pte = mmu_find_pte(vm->page_map, va);
        if (pte && (*pte & MMU_PRESENT) && (!write || (*pte & MMU_WRITABLE)))
            continue;
        if (!fault_locked(vm, va, write))
            return false;
    }
    return true;
}

static int write_locked(struct vm_space* vm, uint64_t addr, const void* src, size_t len)
{
    const uint8_t* p = src;
    while (len) {
        const struct vm_area* a = vm_find(vm, addr);
        if (!a)
            return -EFAULT;
        uint64_t* pte = frame_for(vm, addr & PAGE_MASK, a->prot);
        if (!pte || ((*pte & PTE_COW) && !cow_break(vm, addr & PAGE_MASK, pte)))
            return -ENOMEM;
        const size_t off = addr & ~PAGE_MASK;
        const size_t n = PAGE_SIZE - off < len ? PAGE_SIZE - off : len;
        uint8_t* frame = phys_to_virt(*pte & PTE_FRAME);
        if (p)
            memcpy(frame + off, p, n);
        else
            memset(frame + off, 0, n);
        if (p)
            p += n;
        addr += n;
        len -= n;
    }
    return 0;
}

uint64_t vm_translate(const struct vm_space* vm, const uint64_t addr)
{
    const uint64_t* pte = mmu_find_pte(vm->page_map, addr & PAGE_MASK);
    if (!pte || !(*pte & PTE_FRAME))
        return 0;
    return (*pte & PTE_FRAME) | (addr & ~PAGE_MASK);
}

int vm_map(struct vm_space* vm, const uint64_t addr, const size_t len, const uint32_t prot, const uint32_t flags,
           const bool fixed, uint64_t* out)
{
    spinlock_acquire(&vm->lock);
    const int err = map_locked(vm, addr, len, prot, flags, fixed, out);
    spinlock_release(&vm->lock);
    drain(vm);
    return err;
}

int vm_unmap(struct vm_space* vm, const uint64_t addr, const size_t len)
{
    spinlock_acquire(&vm->lock);
    const int err = unmap_locked(vm, addr, len);
    spinlock_release(&vm->lock);
    drain(vm);
    return err;
}

int vm_protect(struct vm_space* vm, const uint64_t addr, const size_t len, const uint32_t prot)
{
    spinlock_acquire(&vm->lock);
    const int err = protect_locked(vm, addr, len, prot);
    spinlock_release(&vm->lock);
    return err;
}

int vm_discard(struct vm_space* vm, const uint64_t addr, const size_t len)
{
    spinlock_acquire(&vm->lock);
    const int err = discard_locked(vm, addr, len);
    spinlock_release(&vm->lock);
    return err;
}

uint64_t vm_brk(struct vm_space* vm, const uint64_t new_brk)
{
    spinlock_acquire(&vm->lock);
    const uint64_t brk = brk_locked(vm, new_brk);
    spinlock_release(&vm->lock);
    drain(vm);
    return brk;
}

int vm_remap(struct vm_space* vm, const uint64_t old_addr, const size_t old_len, const size_t new_len, const int flags,
             uint64_t* out)
{
    spinlock_acquire(&vm->lock);
    const int err = remap_locked(vm, old_addr, old_len, new_len, flags, out);
    spinlock_release(&vm->lock);
    drain(vm);
    return err;
}

int vm_remap_fixed(struct vm_space* vm, const uint64_t old_addr, const size_t old_len, const size_t new_len,
                   const int flags, const uint64_t new_addr, uint64_t* out)
{
    spinlock_acquire(&vm->lock);
    const int err = remap_fixed_locked(vm, old_addr, old_len, new_len, flags, new_addr, out);
    spinlock_release(&vm->lock);
    drain(vm);
    return err;
}

int vm_sync(struct vm_space* vm, const uint64_t addr, const size_t len)
{
    spinlock_acquire(&vm->lock);
    const int err = sync_locked(vm, addr, len);
    spinlock_release(&vm->lock);
    drain(vm);
    return err;
}

int vm_attach_file(struct vm_space* vm, const uint64_t start, const struct vm_file_ops* ops, void* file,
                   const uint64_t offset)
{
    spinlock_acquire(&vm->lock);
    struct vm_area* a = vm_find(vm, start);
    int err = -EINVAL;
    if (a && a->start == start && !a->file) {
        a->fops = ops;
        a->file = file;
        a->offset = offset;
        ops->acquire(file);
        err = 0;
    }
    spinlock_release(&vm->lock);
    return err;
}

int vm_insert_frame(struct vm_space* vm, const uint64_t va, const uint64_t frame)
{
    spinlock_acquire(&vm->lock);
    const int err = insert_frame_locked(vm, va, frame);
    spinlock_release(&vm->lock);
    return err;
}

bool vm_fault(struct vm_space* vm, const uint64_t addr, const bool write)
{
    spinlock_acquire(&vm->lock);
    const bool ok = fault_locked(vm, addr, write);
    spinlock_release(&vm->lock);
    return ok;
}

bool vm_populate(struct vm_space* vm, const uint64_t addr, const size_t len, const bool write)
{
    spinlock_acquire(&vm->lock);
    const bool ok = populate_locked(vm, addr, len, write);
    spinlock_release(&vm->lock);
    return ok;
}

int vm_write(struct vm_space* vm, const uint64_t addr, const void* src, const size_t len)
{
    spinlock_acquire(&vm->lock);
    const int err = write_locked(vm, addr, src, len);
    spinlock_release(&vm->lock);
    return err;
}
