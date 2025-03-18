#include "vga.h"
#include <utils/str/str.h>


size_t terminal_row = 0;
size_t terminal_column = 0;
uint8_t terminal_color;
volatile uint16_t* terminal_buffer;

uint8_t vga_entry_color(const enum vga_color fg, const enum vga_color bg)
{
    return fg | bg << 4;
}

uint16_t vga_entry(const unsigned char uc, const uint8_t color)
{
    return (uint16_t) uc | (uint16_t) color << 8;
}

void vga_text_clear()
{
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
        }
    }
    terminal_column = 0;
    terminal_row = 0;
}

void vga_text_initialize()
{
    terminal_buffer = (volatile uint16_t*) 0xB8000;
    terminal_color = vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    vga_text_clear();
}

void vga_text_set_color(const uint8_t color)
{
    terminal_color = color;
}

void vga_text_put_at(const char c, const uint8_t color, const size_t x, const size_t y)
{
    terminal_buffer[y * VGA_WIDTH + x] = vga_entry(c, color);
}

void vga_text_put_char(const char c)
{
    vga_text_put_at(c, terminal_color, terminal_column, terminal_row);
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_row = 0;
    }
}

void vga_text_write_char(const char c)
{
    vga_text_put_char(c);
}

void vga_text_write_number(uint64_t n)
{
    char buffer[21];
    buffer[20] = '\0';

    if (n == 0) {
        vga_text_write_string("0");
        return;
    }

    int i = 19;
    while (n > 0 && i >= 0) {
        buffer[i--] = '0' + n % 10;
        n /= 10;
    }
    vga_text_write_string(&buffer[i + 1]);
}

void vga_text_write(const char* data, const size_t size)
{
    for (size_t i = 0; i < size; i++)
        vga_text_put_char(data[i]);
}

void vga_text_write_string(const char* data)
{
    vga_text_write(data, strlen(data));
}

void vga_text_write_hex(uint64_t n)
{
    char buffer[17];
    buffer[16] = '\0';
    for (int i = 0; i < 16; i++) {
        buffer[15 - i] = "0123456789ABCDEF"[n % 16];
        n /= 16;
    }
    vga_text_write_string(buffer);
}

void vga_text_clear_line(const size_t line)
{
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        terminal_buffer[line * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
    }
}

void vga_text_backspace() {
    if (terminal_column == 0) {
        if (terminal_row == 0) {
            return;
        }
        terminal_column = VGA_WIDTH - 1;
        terminal_row--;
    } else {
        terminal_column--;
    }
    vga_text_put_char(' ');
    terminal_column--;
}

void vga_text_newline()
{
    terminal_column = 0;
    if (++terminal_row == VGA_HEIGHT)
        terminal_row = 0;
}