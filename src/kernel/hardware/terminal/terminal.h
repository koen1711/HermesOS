#ifndef TERMINAL_H
#define TERMINAL_H
#include <os/stddef.h>

typedef enum
{
    VGA_TEXT_MODE,
} terminal_mode;

typedef struct
{
    const char* content;
    size_t length;
    size_t capacity;
} terminal_contents;

#endif //TERMINAL_H
