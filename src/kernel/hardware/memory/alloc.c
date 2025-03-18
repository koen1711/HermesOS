#include "alloc.h"

extern void* _kernel_virtual_end;

void* malloc(const uintptr_t size) {
    static void* next = &_kernel_virtual_end;
    void* result = next;
    next += size;
    return result;
}

void free(void* ptr) {
    
}

void memcpy(void* dest, void* src, const uintptr_t size) {
    for (uintptr_t i = 0; i < size; i++) {
        ((uint8_t*)dest)[i] = ((uint8_t*)src)[i];
    }
}