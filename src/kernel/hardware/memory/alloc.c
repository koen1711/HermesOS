#include "alloc.h"

#include "mmu.h"
#include "pmm.h"
#include <hardware/interrupts/panic.h>
#include <utils/sync/spinlock.h>

#define DEFAULT_BLOCK_SIZE 4096
#define BLOCKS 8192

extern void* _kernel_virtual_end;

struct memory_block_s
{
    uint64_t size;
    struct memory_block_s* next;
} __attribute__((packed));

typedef struct memory_block_s memory_block_t;

memory_block_t* first_free_block = nullptr;
static spinlock_t heap_lock = { .name = "heap" };

void memory_initialize(const multiboot_info_t mb_info) {
    pmm_initialize(mb_info);
    mmu_initialize();

    first_free_block = (memory_block_t*)vmalloc(DEFAULT_BLOCK_SIZE * BLOCKS);

    if (first_free_block == nullptr) {
        panic("Failed to allocate initial memory block", __FILE__, __LINE__);
        return;
    }

    first_free_block->size = DEFAULT_BLOCK_SIZE * BLOCKS;
    first_free_block->next = nullptr;
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

void* malloc(const uintptr_t size) {
    if (size == 0) return nullptr;

    const uintptr_t aligned_size = (size + 7) & ~7;

    uint64_t flags;
    spinlock_acquire_irqsave(&heap_lock, &flags);
    if (first_free_block == nullptr) {
        panic("No free memory blocks available", __FILE__, __LINE__);
        return nullptr;
    }

    memory_block_t* current = first_free_block;
    memory_block_t* previous = nullptr;

    while (current != nullptr) {
        if (current->size >= aligned_size) {
            const uintptr_t total_needed = aligned_size + sizeof(memory_block_t);

            if (current->size >= total_needed + 8) {
                uint8_t* split_addr = (uint8_t*)current + sizeof(memory_block_t) + aligned_size;
                const auto split_block = (memory_block_t*)split_addr;

                split_block->size = current->size - total_needed;
                split_block->next = current->next;

                current->size = aligned_size;



                if (previous != nullptr)
                {
                    previous->next = split_block;
                } else
                    first_free_block = split_block;

            } else {
                if (previous != nullptr)
                    previous->next = current->next;
                else
                    first_free_block = current->next;
            }

            spinlock_release_irqrestore(&heap_lock, flags);
            uint8_t* current_address = (uint8_t*)current + sizeof(memory_block_t);
            memset(current_address, 0, aligned_size);
            return current_address;
        }

        previous = current;
        current = current->next;
    }

    spinlock_release_irqrestore(&heap_lock, flags);
    return nullptr;
}

static void coalesce_free_blocks() {
    memory_block_t* current = first_free_block;

    while (current != nullptr && current->next != nullptr) {
        const uint8_t* current_end = (uint8_t*)current + sizeof(memory_block_t) + current->size;
        const uint8_t* next_start = (uint8_t*)current->next;

        if (current_end == next_start) {
            const memory_block_t* next_block = current->next;
            current->size += sizeof(memory_block_t) + next_block->size;
            current->next = next_block->next;
        } else {
            current = current->next;
        }
    }
}

void free(void* ptr) {
    if (ptr == nullptr) return;

    const auto block_to_free = (memory_block_t*)((uint8_t*)ptr - sizeof(memory_block_t));

    uint64_t flags;
    spinlock_acquire_irqsave(&heap_lock, &flags);
    if (!first_free_block || block_to_free < first_free_block) {
        block_to_free->next = first_free_block;
        first_free_block = block_to_free;
    } else {
        memory_block_t* current = first_free_block;
        while (current->next && current->next < block_to_free) {
            current = current->next;
        }
        block_to_free->next = current->next;
        current->next = block_to_free;
    }

    coalesce_free_blocks();
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
    for (size_t i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[n] = '\0';
}

void* realloc(void* ptr, const uintptr_t size) {
    if (ptr == nullptr) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return nullptr;
    }
    void* new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, size);
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