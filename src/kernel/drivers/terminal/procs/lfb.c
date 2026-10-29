#include "lfb.h"

#include "drivers/terminal/font.h"
#include "drivers/video/LFB.h"
#include "hardware/memory/alloc.h"

static constexpr uint32_t palette[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};

static const framebuffer_t* fb = nullptr;
static vt_cell* shadow = nullptr;
static terminal_backend lfb_backend;
static size_t cursor_x = 0;
static size_t cursor_y = 0;
static bool cursor_visible = false;

static void lfb_text_render(const size_t x, const size_t y, const vt_cell cell, const bool cursor)
{
    const uint8_t* glyph = font_glyphs[cell.ch & 0x7F];
    const uint32_t fg = palette[cell.fg & 0xF];
    const uint32_t bg = palette[cell.bg & 0xF];
    uint32_t* line = fb->addr + y * FONT_HEIGHT * fb->pitch + x * FONT_WIDTH;

    for (size_t row = 0; row < FONT_HEIGHT; row++, line += fb->pitch) {
        uint8_t bits = glyph[row];
        if ((cursor && row >= FONT_HEIGHT - 2) || ((cell.attr & VT_ATTR_UNDERLINE) && row == FONT_HEIGHT - 1))
            bits = 0xFF;
        for (size_t col = 0; col < FONT_WIDTH; col++)
            line[col] = bits & (0x80 >> col) ? fg : bg;
    }
}

static bool lfb_text_is_cursor(const size_t x, const size_t y)
{
    return cursor_visible && x == cursor_x && y == cursor_y;
}

static void lfb_text_draw(const size_t x, const size_t y, const vt_cell cell)
{
    if (x >= lfb_backend.cols || y >= lfb_backend.rows) return;

    vt_cell* old = &shadow[y * lfb_backend.cols + x];
    if (old->ch == cell.ch && old->fg == cell.fg && old->bg == cell.bg && old->attr == cell.attr)
        return;
    *old = cell;
    lfb_text_render(x, y, cell, lfb_text_is_cursor(x, y));
}

static void lfb_text_cursor(const size_t x, const size_t y, const bool visible)
{
    if (x == cursor_x && y == cursor_y && visible == cursor_visible)
        return;
    if (cursor_visible)
        lfb_text_render(cursor_x, cursor_y, shadow[cursor_y * lfb_backend.cols + cursor_x], false);

    cursor_x = x < lfb_backend.cols ? x : lfb_backend.cols - 1;
    cursor_y = y < lfb_backend.rows ? y : lfb_backend.rows - 1;
    cursor_visible = visible;
    if (cursor_visible)
        lfb_text_render(cursor_x, cursor_y, shadow[cursor_y * lfb_backend.cols + cursor_x], true);
}

const terminal_backend* lfb_text_backend()
{
    fb = lfb_framebuffer();
    if (fb == nullptr)
        return nullptr;

    const size_t cols = fb->width / FONT_WIDTH;
    const size_t rows = fb->height / FONT_HEIGHT;
    if (shadow == nullptr)
        shadow = malloc(cols * rows * sizeof(vt_cell));
    if (shadow == nullptr)
        return nullptr;
    memset(shadow, 0xFF, cols * rows * sizeof(vt_cell));
    cursor_visible = false;

    lfb_backend = (terminal_backend){
        .cols = cols,
        .rows = rows,
        .draw = lfb_text_draw,
        .cursor = lfb_text_cursor,
    };
    return &lfb_backend;
}
