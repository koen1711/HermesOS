#ifndef VM_H
#define VM_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <os/stdbool.h>
#include <utils/queue/queue.h>
#include <hardware/memory/mmu.h>

// the first 1GB is the kernel's identity map; non-PIC programs link at USER_IMAGE_BASE
#define USER_ADDR_MIN       0x0000000040000000ULL
#define USER_IMAGE_BASE     USER_ADDR_MIN
#define USER_ADDR_LIMIT     0x0000800000000000ULL
#define USER_PIE_BASE       0x0000100000000000ULL
#define USER_INTERP_BASE    0x0000200000000000ULL
#define USER_MMAP_BASE      0x0000300000000000ULL
#define USER_STACK_TOP      0x00007FFFFFFFF000ULL
#define USER_STACK_LIMIT    (8 * 1024 * 1024)

#define PAGE_MASK           (~(uint64_t)(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(x)    (((uint64_t)(x) + PAGE_SIZE - 1) & PAGE_MASK)

// same bits as PROT_READ, PROT_WRITE, PROT_EXEC
enum vm_prot {
    VM_NONE = 0,
    VM_READ = 1,
    VM_WRITE = 2,
    VM_EXEC = 4
};

enum vm_area_flags {
    VMA_STACK = 0x1,
    VMA_HEAP = 0x2
};

struct vm_area {
    struct dlist_node link;
    uint64_t start;
    uint64_t end;
    uint32_t prot;
    uint32_t flags;
};

// An address space, shared by the threads of a process
struct vm_space {
    uint64_t page_map;
    int ref_count;
    struct dlist areas;     // sorted by start, never overlapping
    uint64_t brk_start;
    uint64_t brk;
    uint64_t mmap_base;
};

struct vm_space* vm_create();
struct vm_space* vm_clone(const struct vm_space* src);
void vm_acquire(struct vm_space* vm);
void vm_release(struct vm_space* vm);

struct vm_area* vm_find(const struct vm_space* vm, uint64_t addr);
bool vm_range_free(const struct vm_space* vm, uint64_t start, uint64_t end);

// Reserve [addr, addr + len); pages are allocated on first touch. fixed replaces what is there.
int vm_map(struct vm_space* vm, uint64_t addr, size_t len, uint32_t prot, uint32_t flags, bool fixed,
           uint64_t* out);
int vm_unmap(struct vm_space* vm, uint64_t addr, size_t len);
int vm_protect(struct vm_space* vm, uint64_t addr, size_t len, uint32_t prot);
int vm_discard(struct vm_space* vm, uint64_t addr, size_t len);
uint64_t vm_brk(struct vm_space* vm, uint64_t new_brk);

bool vm_fault(struct vm_space* vm, uint64_t addr, bool write);
bool vm_populate(struct vm_space* vm, uint64_t addr, size_t len, bool write);

// Copy into the address space through the physical frames, ignoring protection (exec, clone)
int vm_write(struct vm_space* vm, uint64_t addr, const void* src, size_t len);
uint64_t vm_translate(const struct vm_space* vm, uint64_t addr);

#endif //VM_H
