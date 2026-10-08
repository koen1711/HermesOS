#ifndef TERMINAL_H
#define TERMINAL_H
#include <os/stddef.h>

typedef enum
{
    VGA_TEXT_MODE,
    VGA_LFB_MODE,
} terminal_mode;

typedef struct
{
    const char* content;
    size_t length;
    size_t capacity;
} terminal_contents;


void terminal_initialize(terminal_mode mode);
void terminal_switch_mode(terminal_mode mode);
void terminal_printf(const char* format, ...);
void terminal_update();

#endif //TERMINAL_H
