#ifndef OS_VGA_H
#define OS_VGA_H

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

#include <stdint.h>
#include <stddef.h>

enum vga_color {
    VGA_COLOR_BLACK = 0,
    VGA_COLOR_BLUE = 1,
    VGA_COLOR_GREEN = 2,
    VGA_COLOR_CYAN = 3,
    VGA_COLOR_RED = 4,
    VGA_COLOR_MAGENTA = 5,
    VGA_COLOR_BROWN = 6,
    VGA_COLOR_LIGHT_GREY = 7,
    VGA_COLOR_DARK_GREY = 8,
    VGA_COLOR_LIGHT_BLUE = 9,
    VGA_COLOR_LIGHT_GREEN = 10,
    VGA_COLOR_LIGHT_CYAN = 11,
    VGA_COLOR_LIGHT_RED = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_LIGHT_BROWN = 14,
    VGA_COLOR_WHITE = 15,
};

void vga_text_initialize();
void vga_text_set_color(uint8_t color);
void vga_text_write_char(char c);
void vga_text_write_hex(uint64_t n);
void vga_text_write_number(uint64_t n);
void vga_text_write(const char* data, size_t size);
void vga_text_write_string(const char* data);

void vga_text_clear();
void vga_text_clear_line(size_t line);
void vga_text_backspace();

void vga_text_newline();

#endif //OS_VGA_H