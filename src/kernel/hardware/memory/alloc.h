#ifndef ALLOCATION_H
#define ALLOCATION_H

#include <multiboot.h>

#include <os/stdbool.h>
#include <os/stdint.h>



void memory_initialize(multiboot_info_t mb_info);

void* malloc(uintptr_t size);
void* malloc_safe(uintptr_t size);
void free(void* ptr);

void* realloc(void* ptr, uintptr_t size);
void* calloc(uintptr_t num, uintptr_t size);

void* memcpy(void* dest, const void* src, uintptr_t size);
#define memcpy(dest, src, size) __builtin_memcpy((dest), (src), (size))
int memcmp(const void* v1, const void* v2, uintptr_t size);
#define memcmp(v1, v2, size) __builtin_memcmp((v1), (v2), (size))
void* memset(void* dest, int value, uintptr_t size);
#define memset(dest, value, size) __builtin_memset((dest), (value), (size))
void* memmove(void* dest, const void* src, uintptr_t size);
#define memmove(dest, src, size) __builtin_memmove((dest), (src), (size))


#endif //ALLOCATION_H
