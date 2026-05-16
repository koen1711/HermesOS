#ifndef VECTOR_H
#define VECTOR_H

#include <os/stdint.h>

typedef struct {
    int x;
    int y;
} vector_2d;

typedef struct {
    uint64_t x;
    uint64_t y;
} vector_2d_uint;

#endif //VECTOR_H
