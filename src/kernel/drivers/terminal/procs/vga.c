#include "vga.h"
#include <utils/str/str.h>

#include "hardware/port/ports.h"
#include <utils/vector/vector.h>

terminal_contents* terminal = NULL;
vga_color terminal_color = VGA_COLOR_WHITE;

uint16_t* terminal_buffer = (uint16_t*) 0xB8000;

uint8_t vga_entry_color(const vga_color fg, const vga_color bg)
{
    return fg | bg << 4;
}

uint16_t vga_entry(const unsigned char uc, const uint8_t color)
{
    return (uint16_t) uc | (uint16_t) color << 8;
}

void vga_text_initialize(terminal_contents* term)
{
    terminal = term;
    terminal_color = vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

void vga_text_put_at(const char c, const uint8_t color, const size_t x, const size_t y)
{
    terminal_buffer[y * VGA_WIDTH + x] = vga_entry(c, color);
}

void vga_text_put_char(const char c, const size_t x, const size_t y)
{
    if (x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    vga_text_put_at(c, terminal_color, x, y);
}

void vga_text_update()
{
    if (terminal == NULL || terminal->content == NULL) return;

    // Update the VGA text mode terminal
    size_t row = 0;
    size_t column = 0;
    for (size_t i = 0; i < terminal->length; i++) {
        if (terminal->content[i] == '\n') {
            row++;
            column = 0;
            continue;
        }
        vga_text_put_char(terminal->content[i], column, row);
        column++;
    }
}

void vga_text_toggle_cursor(const bool enable)
{
    if (enable) {
        // Enable cursor
        port_write_u8(0x3D4, 0x0A);
        port_write_u8(0x3D5, 0x20);
        port_write_u8(0x3D4, 0x0B);
        port_write_u8(0x3D5, 0x00);
    } else {
        // Disable cursor
        port_write_u8(0x3D4, 0x0A);
        port_write_u8(0x3D5, 0x20);
    }
}

// void vga_text_set_cursor_position(const size_t x, const size_t y)
// {
//
//
//     const uint16_t position = y * VGA_WIDTH + x;
//     port_write_u8(0x3D4, 0x0E);
//     port_write_u8(0x3D5, (uint8_t) (position >> 8));
//     port_write_u8(0x3D4, 0x0F);
//     port_write_u8(0x3D5, (uint8_t) (position & 0xFF));
// }