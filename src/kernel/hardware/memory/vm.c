#include "vm.h"

#include "alloc.h"
#include "pmm.h"
#include <hardware/smp/smp.h>
#include <os/errno.h>

#define PTE_FRAME 0x000FFFFFFFFFF000ULL

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

static int area_split(struct vm_space* vm, struct vm_area* a, const uint64_t at)
{
    struct vm_area* b = area_new(at, a->end, a->prot, a->flags);
    if (!b)
        return -ENOMEM;
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
    memset(page, 0, PAGE_SIZE);
    if (map_page_in(vm->page_map, va, (uint64_t)page, pte_flags(prot)) < 0) {
        free_physical_page(page);
        return nullptr;
    }
    return mmu_find_pte(vm->page_map, va);
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
    vm->ref_count = 1;
    vm->mmap_base = USER_MMAP_BASE;
    dlist_init(&vm->areas);
    return vm;
}

struct vm_space* vm_clone(const struct vm_space* src)
{
    struct vm_space* vm = vm_create();
    if (!vm)
        return nullptr;
    vm->brk_start = src->brk_start;
    vm->brk = src->brk;
    vm->mmap_base = src->mmap_base;

    for (struct dlist_node* n = dlist_first(&src->areas); n; n = dlist_next(n)) {
        const struct vm_area* a = area_of(n);
        struct vm_area* copy = area_new(a->start, a->end, a->prot, a->flags);
        if (!copy) {
            vm_release(vm);
            return nullptr;
        }
        dlist_insert_tail(&vm->areas, &copy->link);

        for (uint64_t va = a->start; va < a->end; va += PAGE_SIZE) {
            const uint64_t* pte = mmu_find_pte(src->page_map, va);
            if (!pte || !(*pte & PTE_FRAME))
                continue;
            if (a->flags & VMA_SHARED) {
                if (map_page_in(vm->page_map, va, *pte & PTE_FRAME, *pte & ~PTE_FRAME) < 0) {
                    vm_release(vm);
                    return nullptr;
                }
                pmm_share_page((void*)(*pte & PTE_FRAME));
                continue;
            }
            void* page = alloc_physical_page();
            if (!page) {
                vm_release(vm);
                return nullptr;
            }
            memcpy(page, (const void*)(*pte & PTE_FRAME), PAGE_SIZE);
            if (map_page_in(vm->page_map, va, (uint64_t)page, *pte & ~PTE_FRAME) < 0) {
                free_physical_page(page);
                vm_release(vm);
                return nullptr;
            }
        }
    }
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
        dlist_remove(&vm->areas, n);
        free(area_of(n));
    }
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

int vm_map(struct vm_space* vm, const uint64_t addr, size_t len, const uint32_t prot, const uint32_t flags,
           const bool fixed, uint64_t* out)
{
    len = PAGE_ALIGN_UP(len);
    if (len == 0 || (addr & ~PAGE_MASK))
        return -EINVAL;

    uint64_t start = addr;
    if (fixed) {
        if (!user_range(start, start + len))
            return -EINVAL;
        const int err = vm_unmap(vm, start, len);
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

int vm_unmap(struct vm_space* vm, const uint64_t addr, const size_t len)
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
            free_frames(vm, a->start, a->end);
            dlist_remove(&vm->areas, n);
            free(a);
        }
        n = next;
    }
    smp_tlb_shootdown();
    return 0;
}

int vm_protect(struct vm_space* vm, const uint64_t addr, const size_t len, const uint32_t prot)
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
            *pte = (*pte & PTE_FRAME) | pte_flags(prot);
            invalidate(va);
        }
    }
    smp_tlb_shootdown();
    return 0;
}

int vm_discard(struct vm_space* vm, const uint64_t addr, const size_t len)
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

uint64_t vm_brk(struct vm_space* vm, const uint64_t new_brk)
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
        } else if (vm_map(vm, old_end, new_end - old_end, VM_READ | VM_WRITE, VMA_HEAP, true, nullptr) < 0) {
            return vm->brk;
        }
    } else if (new_end < old_end) {
        vm_unmap(vm, new_end, old_end - new_end);
    }
    vm->brk = new_brk;
    return vm->brk;
}

bool vm_fault(struct vm_space* vm, const uint64_t addr, const bool write)
{
    const struct vm_area* a = vm_find(vm, addr);
    if (!a || !(a->prot & (write ? VM_WRITE : VM_READ)))
        return false;

    const uint64_t va = addr & PAGE_MASK;
    uint64_t* pte = mmu_find_pte(vm->page_map, va);
    if (pte && (*pte & MMU_PRESENT)) {
        invalidate(va);
        return !write || (*pte & MMU_WRITABLE);
    }
    pte = frame_for(vm, va, a->prot);
    if (!pte)
        return false;
    *pte = (*pte & PTE_FRAME) | pte_flags(a->prot);
    invalidate(va);
    return true;
}

bool vm_populate(struct vm_space* vm, const uint64_t addr, const size_t len, const bool write)
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
        if (pte && (*pte & MMU_PRESENT))
            continue;
        if (!vm_fault(vm, va, write))
            return false;
    }
    return true;
}

int vm_write(struct vm_space* vm, uint64_t addr, const void* src, size_t len)
{
    const uint8_t* p = src;
    while (len) {
        const struct vm_area* a = vm_find(vm, addr);
        if (!a)
            return -EFAULT;
        const uint64_t* pte = frame_for(vm, addr & PAGE_MASK, a->prot);
        if (!pte)
            return -ENOMEM;
        const size_t off = addr & ~PAGE_MASK;
        const size_t n = PAGE_SIZE - off < len ? PAGE_SIZE - off : len;
        if (p)
            memcpy((uint8_t*)(*pte & PTE_FRAME) + off, p, n);
        else
            memset((uint8_t*)(*pte & PTE_FRAME) + off, 0, n);
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
