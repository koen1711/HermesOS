#include "alloc.h"

#include "mmu.h"
#include "pmm.h"
#include <hardware/interrupts/panic.h>
#include <utils/sync/spinlock.h>

#define HEAP_INITIAL_SIZE (32ULL * 1024 * 1024)
#define HEAP_GROW_SIZE (4ULL * 1024 * 1024)
#define LARGE_ALLOC_THRESHOLD (32 * 1024)
#define BLOCK_USED ((memory_block_t*)0x4845415055534544ULL)
#define BLOCK_LARGE ((memory_block_t*)0x484541504C524745ULL)

extern void* _kernel_virtual_end;

struct memory_block_s
{
    uint64_t size;
    struct memory_block_s* next;
} __attribute__((packed));

typedef struct memory_block_s memory_block_t;

memory_block_t* first_free_block = nullptr;
static spinlock_t heap_lock = { .name = "heap" };

static void insert_free_locked(memory_block_t* block) {
    memory_block_t* previous = nullptr;
    memory_block_t* next = first_free_block;
    while (next && next < block) {
        previous = next;
        next = next->next;
    }

    block->next = next;
    if (next && (uint8_t*)block + sizeof(memory_block_t) + block->size == (uint8_t*)next) {
        block->size += sizeof(memory_block_t) + next->size;
        block->next = next->next;
    }

    if (!previous) {
        first_free_block = block;
    } else if ((uint8_t*)previous + sizeof(memory_block_t) + previous->size == (uint8_t*)block) {
        previous->size += sizeof(memory_block_t) + block->size;
        previous->next = block->next;
    } else {
        previous->next = block;
    }
}

static bool heap_add_chunk(const uint64_t bytes) {
    memory_block_t* chunk = alloc_kernel_pages(bytes / PAGE_SIZE);
    if (!chunk) return false;
    chunk->size = bytes - sizeof(memory_block_t);

    uint64_t flags;
    spinlock_acquire_irqsave(&heap_lock, &flags);
    insert_free_locked(chunk);
    spinlock_release_irqrestore(&heap_lock, flags);
    return true;
}

static bool heap_grow(const uintptr_t aligned_size) {
    const uint64_t needed = PAGE_ALIGN_UP(aligned_size + sizeof(memory_block_t));
    if (needed < HEAP_GROW_SIZE && heap_add_chunk(HEAP_GROW_SIZE)) return true;
    return heap_add_chunk(needed);
}

void memory_initialize(const multiboot_info_t mb_info) {
    pmm_initialize(mb_info);
    mmu_initialize();

    if (!heap_add_chunk(HEAP_INITIAL_SIZE)) {
        panic("Failed to allocate initial memory block", __FILE__, __LINE__);
    }
}

uint64_t get_amount_of_free_memory() {
    uint64_t flags;
    spinlock_acquire_irqsave(&heap_lock, &flags);
    uint64_t total_free = 0;
    for (const memory_block_t* current = first_free_block; current != nullptr; current = current->next) {
        total_free += current->size;
    }
    spinlock_release_irqrestore(&heap_lock, flags);
    return total_free;
}

static void* heap_take(const uintptr_t aligned_size) {
    uint64_t flags;
    spinlock_acquire_irqsave(&heap_lock, &flags);

    memory_block_t* current = first_free_block;
    memory_block_t* previous = nullptr;

    while (current != nullptr) {
        if (current->size >= aligned_size) {
            const uintptr_t total_needed = aligned_size + sizeof(memory_block_t);
            memory_block_t* next = current->next;

            if (current->size >= total_needed + 8) {
                const auto split_block = (memory_block_t*)((uint8_t*)current + total_needed);
                split_block->size = current->size - total_needed;
                split_block->next = current->next;
                current->size = aligned_size;
                next = split_block;
            }

            if (previous != nullptr)
                previous->next = next;
            else
                first_free_block = next;

            current->next = BLOCK_USED;
            spinlock_release_irqrestore(&heap_lock, flags);
            return (uint8_t*)current + sizeof(memory_block_t);
        }

        previous = current;
        current = current->next;
    }

    spinlock_release_irqrestore(&heap_lock, flags);
    return nullptr;
}

static void* large_alloc(const uintptr_t aligned_size) {
    const uint64_t bytes = PAGE_ALIGN_UP(aligned_size + sizeof(memory_block_t));
    memory_block_t* block = alloc_kernel_pages(bytes / PAGE_SIZE);
    if (!block) return nullptr;
    block->size = bytes - sizeof(memory_block_t);
    block->next = BLOCK_LARGE;
    return (uint8_t*)block + sizeof(memory_block_t);
}

void* malloc(const uintptr_t size) {
    if (size == 0 || size > (1ULL << 40)) return nullptr;

    const uintptr_t aligned_size = (size + 7) & ~7;

    void* ptr = aligned_size >= LARGE_ALLOC_THRESHOLD ? large_alloc(aligned_size) : nullptr;
    while (!ptr) {
        ptr = heap_take(aligned_size);
        if (!ptr && !heap_grow(aligned_size)) return nullptr;
    }

    memset(ptr, 0, ((memory_block_t*)ptr - 1)->size);
    return ptr;
}

void free(void* ptr) {
    if (ptr == nullptr) return;

    const auto block = (memory_block_t*)((uint8_t*)ptr - sizeof(memory_block_t));

    if (block->next == BLOCK_LARGE) {
        block->next = nullptr;
        free_kernel_pages(block, (block->size + sizeof(memory_block_t)) / PAGE_SIZE);
        return;
    }
    if (block->next != BLOCK_USED) return;

    uint64_t flags;
    spinlock_acquire_irqsave(&heap_lock, &flags);
    insert_free_locked(block);
    spinlock_release_irqrestore(&heap_lock, flags);
}

void memcpy(void* dest, const void* src, const uintptr_t size) {
    uintptr_t qwords = size / 8;
    uintptr_t bytes = size % 8;
    asm volatile("rep movsq" : "+D"(dest), "+S"(src), "+c"(qwords) : : "memory");
    asm volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(bytes) : : "memory");
}

bool memcmp(void* v1, void* v2, const uintptr_t size) {
    for (uintptr_t i = 0; i < size; i++) {
        if (((uint8_t*)v1)[i] != ((uint8_t*)v2)[i]) {
            return true;
        }
    }
    return false;
}

void* memset(void* dest, const int value, const uintptr_t size) {
    void* d = dest;
    const uint64_t pattern = 0x0101010101010101ULL * (uint8_t)value;
    uintptr_t qwords = size / 8;
    uintptr_t bytes = size % 8;
    asm volatile("rep stosq" : "+D"(d), "+c"(qwords) : "a"(pattern) : "memory");
    asm volatile("rep stosb" : "+D"(d), "+c"(bytes) : "a"(pattern) : "memory");
    return dest;
}

void* calloc(const uintptr_t num, const uintptr_t size) {
    if (num && size > (uintptr_t)-1 / num) return nullptr;
    return malloc(num * size);
}

void* memmove(void* dest, const void* src, const uintptr_t size) {
    if (dest <= src || (const uint8_t*)src + size <= (uint8_t*)dest) {
        memcpy(dest, src, size);
    } else if (size) {
        void* d = (uint8_t*)dest + size - 1;
        const void* s = (const uint8_t*)src + size - 1;
        uintptr_t n = size;
        asm volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
    return dest;
}

void strncpy(char* dest, const char* src, const size_t n) {
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
}

void* realloc(void* ptr, const uintptr_t size) {
    if (ptr == nullptr) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return nullptr;
    }
    const memory_block_t* block = (memory_block_t*)((uint8_t*)ptr - sizeof(memory_block_t));
    if (block->next != BLOCK_USED && block->next != BLOCK_LARGE) return nullptr;
    const uint64_t old_size = block->size;
    if (size <= old_size) return ptr;

    void* new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, old_size);
        free(ptr);
    }
    return new_ptr;
}

int strncmp(const char* dest, const char* src, const size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (dest[i] != src[i]) {
            return (unsigned char)dest[i] - (unsigned char)src[i];
        }
    }
    return 0;
}