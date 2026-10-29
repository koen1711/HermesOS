#ifndef TERMINAL_H
#define TERMINAL_H
#include <os/stddef.h>
#include <os/stdint.h>

#include "vt.h"

#define TERMINAL_COUNT 6

typedef enum
{
    VGA_TEXT_MODE,
    VGA_LFB_MODE,
} terminal_mode;

typedef struct
{
    size_t cols;
    size_t rows;
    void (*draw)(size_t x, size_t y, vt_cell cell);
    void (*cursor)(size_t x, size_t y, bool visible);
} terminal_backend;

void terminal_initialize(terminal_mode mode);
void terminal_switch_mode(terminal_mode mode);
void terminal_printf(const char* format, ...);
void terminal_write(int index, const char* buf, size_t len, bool onlcr);
void terminal_switch(int index);
int terminal_active();
void terminal_size(int index, uint16_t* cols, uint16_t* rows);
void terminal_update();

#endif //TERMINAL_H
