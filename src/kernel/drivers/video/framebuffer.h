#ifndef OS_FRAMEBUFFER_H
#define OS_FRAMEBUFFER_H

#include <os/stdint.h>

typedef struct {
    uint32_t* addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
} framebuffer_t;

#endif //OS_FRAMEBUFFER_H
