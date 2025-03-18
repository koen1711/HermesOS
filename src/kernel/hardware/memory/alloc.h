#ifndef ALLOCATION_H
#define ALLOCATION_H

#include <stdint.h>

void* malloc(uintptr_t size);
void free(void* ptr);

void memcpy(void* dest, void* src, uintptr_t size);

#endif //ALLOCATION_H
