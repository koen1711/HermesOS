#include "alloc.h"

#include "mmu.h"
#include "hardware/interrupts/panic.h"
#include "hardware/terminal/stdio.h"

#define DEFAULT_BLOCK_SIZE 4096
#define BLOCKS 8192

extern void* _kernel_virtual_end;

struct memory_block_s
{
    uint64_t size;
    struct memory_block_s* next;
} __attribute__((packed));

typedef struct memory_block_s memory_block_t;

memory_block_t* first_free_block = NULL;  // Pointer to the first free memory block

void memory_initialize() {
    first_free_block = (memory_block_t*)vmalloc(DEFAULT_BLOCK_SIZE * BLOCKS);

    if (first_free_block == NULL) {
        panic("Failed to allocate initial memory block", __FILE__, __LINE__);
        return;
    }

    first_free_block->size = DEFAULT_BLOCK_SIZE * BLOCKS;
    first_free_block->next = NULL;
}

uint64_t get_amount_of_free_memory() {
    uint64_t total_free = 0;
    for (const memory_block_t* current = first_free_block; current != NULL; current = current->next) {
        total_free += current->size;
    }
    return total_free;
}

void get_memory_overview() {
    // Print debug info about memory
    const uint64_t total_size = get_amount_of_free_memory();
    printf("Memory Overview:\n");
    printf("Total Memory: %lu bytes\n", total_size);
    printf("Free Blocks:\n");
    for (const memory_block_t* current = first_free_block; current != NULL; current = current->next) {
        printf(" - Block at %p: %lu bytes\n", (void*)current, current->size);
    }
}

void* malloc(const uintptr_t size) {
    if (size == 0) return NULL;

    const uintptr_t aligned_size = (size + 7) & ~7;

    if (first_free_block == NULL) {
        panic("No free memory blocks available", __FILE__, __LINE__);
        return NULL;
    }

    memory_block_t* current = first_free_block;
    memory_block_t* previous = NULL;

    while (current != NULL) {
        if (current->size >= aligned_size) {
            const uintptr_t total_needed = aligned_size + sizeof(memory_block_t);

            if (current->size >= total_needed + 8) {
                uint8_t* split_addr = (uint8_t*)current + sizeof(memory_block_t) + aligned_size;
                memory_block_t* split_block = (memory_block_t*)split_addr;

                split_block->size = current->size - total_needed;
                split_block->next = current->next;

                current->size = aligned_size;



                if (previous != NULL)
                {
                    previous->next = split_block;
                } else
                    first_free_block = split_block;

            } else {
                if (previous != NULL)
                    previous->next = current->next;
                else
                    first_free_block = current->next;
            }

            uint8_t* current_address = (uint8_t*)current + sizeof(memory_block_t);
            memset(current_address, 0, aligned_size);
            return current_address;
        }

        previous = current;
        current = current->next;
    }

    // Out of memory
    printf("malloc: Out of memory! Requested size: %lu bytes\n", aligned_size);
    if (previous != NULL)
    {
        printf("First free block size: %lu bytes at address %p\n", first_free_block->size, (void*)first_free_block);
        printf("Last free block size: %lu bytes at address %p\n", previous->size, (void*)previous);
        get_memory_overview();
    }
    else
        printf("No free blocks available.\n");

    panic("Out of memory in malloc!", __FILE__, __LINE__);
    return NULL;
}

static void coalesce_free_blocks() {
    memory_block_t* current = first_free_block;

    while (current != NULL && current->next != NULL) {
        const uint8_t* current_end = (uint8_t*)current + sizeof(memory_block_t) + current->size;
        const uint8_t* next_start = (uint8_t*)current->next;

        if (current_end == next_start) {
            // Blocks are adjacent, coalesce them
            const memory_block_t* next_block = current->next;
            current->size += sizeof(memory_block_t) + next_block->size;
            current->next = next_block->next;
        } else {
            current = current->next;
        }
    }
}

void free(void* ptr) {
    if (ptr == NULL) return;

    memory_block_t* block_to_free = (memory_block_t*)((uint8_t*)ptr - sizeof(memory_block_t));

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
}

void memcpy(void* dest, const void* src, const uintptr_t size) {
    for (uintptr_t i = 0; i < size; i++) {
        ((uint8_t*)dest)[i] = ((uint8_t*)src)[i];
    }
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
    for (uintptr_t i = 0; i < size; i++) {
        ((uint8_t*)dest)[i] = (uint8_t)value;
    }
    return dest;
}

void* calloc(const uintptr_t num, const uintptr_t size) {
    void* result = malloc(num * size);
    if (result) {
        for (uintptr_t i = 0; i < num * size; i++) {
            ((uint8_t*)result)[i] = 0;
        }
    }
    return result;
}

void* memmove(void* dest, const void* src, const uintptr_t size) {
    if (dest < src) {
        memcpy(dest, src, size);
    } else {
        for (uintptr_t i = size; i > 0; i--) {
            ((uint8_t*)dest)[i - 1] = ((uint8_t*)src)[i - 1];
        }
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
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
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