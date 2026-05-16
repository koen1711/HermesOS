#ifndef ALLOCATION_H
#define ALLOCATION_H

#include <os/stdbool.h>
#include <os/stddef.h>
#include <os/stdint.h>

void memory_initialize();

void* malloc(uintptr_t size);
void free(void* ptr);

void* calloc(uintptr_t num, uintptr_t size);

void memcpy(void* dest, const void* src, uintptr_t size);
bool memcmp(void* v1, void* v2, uintptr_t size);
void* memset(void* dest, int value, uintptr_t size);
void* memmove(void* dest, const void* src, uintptr_t size);

void* realloc(void* ptr, uintptr_t size);

#endif //ALLOCATION_H
