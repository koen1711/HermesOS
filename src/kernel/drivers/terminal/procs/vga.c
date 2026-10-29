#include "vga.h"

#include "hardware/port/ports.h"

static const auto terminal_buffer = (uint16_t*) 0xB8000;

static constexpr vga_color ansi_to_vga[16] = {
    VGA_COLOR_BLACK, VGA_COLOR_RED, VGA_COLOR_GREEN, VGA_COLOR_BROWN,
    VGA_COLOR_BLUE, VGA_COLOR_MAGENTA, VGA_COLOR_CYAN, VGA_COLOR_LIGHT_GREY,
    VGA_COLOR_DARK_GREY, VGA_COLOR_LIGHT_RED, VGA_COLOR_LIGHT_GREEN, VGA_COLOR_LIGHT_BROWN,
    VGA_COLOR_LIGHT_BLUE, VGA_COLOR_LIGHT_MAGENTA, VGA_COLOR_LIGHT_CYAN, VGA_COLOR_WHITE,
};

static uint8_t vga_entry_color(const vga_color fg, const vga_color bg)
{
    return fg | bg << 4;
}

static uint16_t vga_entry(const unsigned char uc, const uint8_t color)
{
    return (uint16_t) uc | (uint16_t) color << 8;
}

static void vga_text_draw(const size_t x, const size_t y, const vt_cell cell)
{
    if (x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    terminal_buffer[y * VGA_WIDTH + x] = vga_entry(cell.ch, vga_entry_color(ansi_to_vga[cell.fg & 0xF], ansi_to_vga[cell.bg & 0xF]));
}

static void vga_text_cursor(const size_t x, const size_t y, const bool visible)
{
    if (!visible) {
        port_write_u8(0x3D4, 0x0A);
        port_write_u8(0x3D5, 0x20);
        return;
    }

    port_write_u8(0x3D4, 0x0A);
    port_write_u8(0x3D5, (port_read_u8(0x3D5) & 0xC0) | 14);
    port_write_u8(0x3D4, 0x0B);
    port_write_u8(0x3D5, (port_read_u8(0x3D5) & 0xE0) | 15);

    const uint16_t position = y * VGA_WIDTH + x;
    port_write_u8(0x3D4, 0x0F);
    port_write_u8(0x3D5, (uint8_t) (position & 0xFF));
    port_write_u8(0x3D4, 0x0E);
    port_write_u8(0x3D5, (uint8_t) (position >> 8));
}

static const terminal_backend vga_backend = {
    .cols = VGA_WIDTH,
    .rows = VGA_HEIGHT,
    .draw = vga_text_draw,
    .cursor = vga_text_cursor,
};

const terminal_backend* vga_text_backend()
{
    return &vga_backend;
}
