#include "vt.h"

#include <hardware/memory/alloc.h>
#include <os/errno.h>

#define VT_DEFAULT_FG 7
#define VT_DEFAULT_BG 0

enum vt_state {
    VT_NORMAL,
    VT_ESC,
    VT_CSI,
    VT_OSC,
    VT_OSC_ESC,
    VT_CHARSET
};

static vt_cell vt_blank(const vt_t* vt)
{
    const bool reverse = vt->attr & VT_ATTR_REVERSE;
    return (vt_cell){ .ch = ' ', .fg = reverse ? vt->bg : vt->fg, .bg = reverse ? vt->fg : vt->bg };
}

static vt_cell vt_styled(const vt_t* vt, const uint8_t c)
{
    uint8_t fg = vt->fg;
    uint8_t bg = vt->bg;
    if ((vt->attr & VT_ATTR_BOLD) && fg < 8)
        fg += 8;
    if (vt->attr & VT_ATTR_REVERSE) {
        const uint8_t t = fg;
        fg = bg;
        bg = t;
    }
    return (vt_cell){ .ch = c, .fg = fg, .bg = bg, .attr = vt->attr & VT_ATTR_UNDERLINE };
}

static void vt_fill(vt_t* vt, const size_t from, const size_t to)
{
    const vt_cell blank = vt_blank(vt);
    for (size_t i = from; i < to; i++)
        vt->cells[i] = blank;
    if (from < to) {
        for (size_t row = from / vt->cols; row <= (to - 1) / vt->cols; row++)
            vt->dirty[row] = 1;
    }
}

static void vt_scroll_up(vt_t* vt, const size_t top, const size_t bottom, size_t n)
{
    if (n > bottom - top)
        n = bottom - top;
    memmove(&vt->cells[top * vt->cols], &vt->cells[(top + n) * vt->cols], (bottom - top - n) * vt->cols * sizeof(vt_cell));
    vt_fill(vt, (bottom - n) * vt->cols, bottom * vt->cols);
    memset(&vt->dirty[top], 1, bottom - top);
}

static void vt_scroll_down(vt_t* vt, const size_t top, const size_t bottom, size_t n)
{
    if (n > bottom - top)
        n = bottom - top;
    memmove(&vt->cells[(top + n) * vt->cols], &vt->cells[top * vt->cols], (bottom - top - n) * vt->cols * sizeof(vt_cell));
    vt_fill(vt, top * vt->cols, (top + n) * vt->cols);
    memset(&vt->dirty[top], 1, bottom - top);
}

static void vt_linefeed(vt_t* vt)
{
    if (vt->y + 1 >= vt->rows)
        vt_scroll_up(vt, 0, vt->rows, 1);
    else
        vt->y++;
}

static void vt_reverse_index(vt_t* vt)
{
    if (vt->y == 0)
        vt_scroll_down(vt, 0, vt->rows, 1);
    else
        vt->y--;
}

static void vt_reset(vt_t* vt)
{
    vt->x = vt->y = vt->saved_x = vt->saved_y = 0;
    vt->fg = vt->saved_fg = VT_DEFAULT_FG;
    vt->bg = vt->saved_bg = VT_DEFAULT_BG;
    vt->attr = vt->saved_attr = 0;
    vt->wrap_pending = false;
    vt->cursor_visible = true;
    vt->state = VT_NORMAL;
    vt_fill(vt, 0, vt->cols * vt->rows);
}

int vt_init(vt_t* vt, const size_t cols, const size_t rows)
{
    memset(vt, 0, sizeof(*vt));
    vt->cells = malloc(cols * rows * sizeof(vt_cell));
    vt->dirty = malloc(rows);
    if (vt->cells == nullptr || vt->dirty == nullptr) {
        free(vt->cells);
        free(vt->dirty);
        return -ENOMEM;
    }
    vt->cols = cols;
    vt->rows = rows;
    vt_reset(vt);
    return 0;
}

int vt_resize(vt_t* vt, const size_t cols, const size_t rows)
{
    if (cols == vt->cols && rows == vt->rows)
        return 0;
    vt_cell* cells = malloc(cols * rows * sizeof(vt_cell));
    uint8_t* dirty = malloc(rows);
    if (cells == nullptr || dirty == nullptr) {
        free(cells);
        free(dirty);
        return -ENOMEM;
    }

    const size_t shift = vt->y >= rows ? vt->y - rows + 1 : 0;
    const vt_cell blank = vt_blank(vt);
    for (size_t y = 0; y < rows; y++) {
        for (size_t x = 0; x < cols; x++) {
            const size_t oy = y + shift;
            cells[y * cols + x] = oy < vt->rows && x < vt->cols ? vt->cells[oy * vt->cols + x] : blank;
        }
    }
    free(vt->cells);
    free(vt->dirty);
    vt->cells = cells;
    vt->dirty = dirty;
    vt->cols = cols;
    vt->rows = rows;
    vt->y -= shift;
    vt->x = vt->x < cols ? vt->x : cols - 1;
    vt->saved_x = vt->saved_x < cols ? vt->saved_x : cols - 1;
    vt->saved_y = vt->saved_y < rows ? vt->saved_y : rows - 1;
    vt_mark_dirty(vt);
    return 0;
}

void vt_mark_dirty(vt_t* vt)
{
    memset(vt->dirty, 1, vt->rows);
}

static void vt_respond(vt_t* vt, const char* s)
{
    while (*s && vt->response_len < VT_RESPONSE_MAX)
        vt->response[vt->response_len++] = *s++;
}

static void vt_respond_number(vt_t* vt, size_t n)
{
    char digits[20];
    size_t len = 0;
    do {
        digits[len++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (len && vt->response_len < VT_RESPONSE_MAX)
        vt->response[vt->response_len++] = digits[--len];
}

static void vt_save_cursor(vt_t* vt)
{
    vt->saved_x = vt->x;
    vt->saved_y = vt->y;
    vt->saved_fg = vt->fg;
    vt->saved_bg = vt->bg;
    vt->saved_attr = vt->attr;
}

static void vt_restore_cursor(vt_t* vt)
{
    vt->x = vt->saved_x;
    vt->y = vt->saved_y;
    vt->fg = vt->saved_fg;
    vt->bg = vt->saved_bg;
    vt->attr = vt->saved_attr;
    vt->wrap_pending = false;
}

static void vt_control(vt_t* vt, const uint8_t c)
{
    switch (c) {
    case '\n':
    case '\v':
    case '\f':
        vt->wrap_pending = false;
        vt_linefeed(vt);
        break;
    case '\r':
        vt->wrap_pending = false;
        vt->x = 0;
        break;
    case '\b':
        vt->wrap_pending = false;
        if (vt->x > 0)
            vt->x--;
        break;
    case '\t':
        vt->wrap_pending = false;
        vt->x = (vt->x / 8 + 1) * 8;
        if (vt->x >= vt->cols)
            vt->x = vt->cols - 1;
        break;
    case 0x1B:
        vt->state = VT_ESC;
        break;
    default:
        break;
    }
}

static void vt_put_char(vt_t* vt, const uint8_t c)
{
    if (vt->wrap_pending) {
        vt->wrap_pending = false;
        vt->x = 0;
        vt_linefeed(vt);
    }
    vt->cells[vt->y * vt->cols + vt->x] = vt_styled(vt, c);
    vt->dirty[vt->y] = 1;
    if (vt->x + 1 >= vt->cols)
        vt->wrap_pending = true;
    else
        vt->x++;
}

static int vt_param(const vt_t* vt, const size_t i, const int def)
{
    return i < vt->param_count && vt->params[i] != 0 ? vt->params[i] : def;
}

static size_t vt_clamp(const int v, const size_t max)
{
    if (v < 0)
        return 0;
    return (size_t)v >= max ? max - 1 : (size_t)v;
}

static int vt_sgr_extended(vt_t* vt, size_t i, uint8_t* out)
{
    const int kind = vt_param(vt, i + 1, 0);
    if (kind == 5) {
        const int idx = vt_param(vt, i + 2, 0);
        if (idx < 16)
            *out = (uint8_t)idx;
        return 2;
    }
    return kind == 2 ? 4 : 1;
}

static void vt_sgr(vt_t* vt)
{
    if (vt->param_count == 0) {
        vt->fg = VT_DEFAULT_FG;
        vt->bg = VT_DEFAULT_BG;
        vt->attr = 0;
        return;
    }
    for (size_t i = 0; i < vt->param_count; i++) {
        const int p = vt->params[i];
        if (p == 0) {
            vt->fg = VT_DEFAULT_FG;
            vt->bg = VT_DEFAULT_BG;
            vt->attr = 0;
        }
        else if (p == 1)
            vt->attr |= VT_ATTR_BOLD;
        else if (p == 4)
            vt->attr |= VT_ATTR_UNDERLINE;
        else if (p == 7)
            vt->attr |= VT_ATTR_REVERSE;
        else if (p == 22)
            vt->attr &= ~VT_ATTR_BOLD;
        else if (p == 24)
            vt->attr &= ~VT_ATTR_UNDERLINE;
        else if (p == 27)
            vt->attr &= ~VT_ATTR_REVERSE;
        else if (p >= 30 && p <= 37)
            vt->fg = (uint8_t)(p - 30);
        else if (p == 38)
            i += vt_sgr_extended(vt, i, &vt->fg);
        else if (p == 39)
            vt->fg = VT_DEFAULT_FG;
        else if (p >= 40 && p <= 47)
            vt->bg = (uint8_t)(p - 40);
        else if (p == 48)
            i += vt_sgr_extended(vt, i, &vt->bg);
        else if (p == 49)
            vt->bg = VT_DEFAULT_BG;
        else if (p >= 90 && p <= 97)
            vt->fg = (uint8_t)(p - 90 + 8);
        else if (p >= 100 && p <= 107)
            vt->bg = (uint8_t)(p - 100 + 8);
    }
}

static void vt_shift_line(vt_t* vt, const size_t n, const bool insert)
{
    vt_cell* line = &vt->cells[vt->y * vt->cols];
    const size_t count = n < vt->cols - vt->x ? n : vt->cols - vt->x;
    const size_t keep = vt->cols - vt->x - count;
    if (insert) {
        memmove(&line[vt->x + count], &line[vt->x], keep * sizeof(vt_cell));
        vt_fill(vt, vt->y * vt->cols + vt->x, vt->y * vt->cols + vt->x + count);
    }
    else {
        memmove(&line[vt->x], &line[vt->x + count], keep * sizeof(vt_cell));
        vt_fill(vt, vt->y * vt->cols + vt->cols - count, (vt->y + 1) * vt->cols);
    }
    vt->dirty[vt->y] = 1;
}

static void vt_csi(vt_t* vt, const uint8_t c)
{
    const size_t pos = vt->y * vt->cols + vt->x;
    const int n = vt_param(vt, 0, 1);

    if (c != 'm' && c != 'n' && c != 'c')
        vt->wrap_pending = false;

    switch (c) {
    case 'A':
        vt->y = vt_clamp((int)vt->y - n, vt->rows);
        break;
    case 'B':
    case 'e':
        vt->y = vt_clamp((int)vt->y + n, vt->rows);
        break;
    case 'C':
    case 'a':
        vt->x = vt_clamp((int)vt->x + n, vt->cols);
        break;
    case 'D':
        vt->x = vt_clamp((int)vt->x - n, vt->cols);
        break;
    case 'E':
        vt->y = vt_clamp((int)vt->y + n, vt->rows);
        vt->x = 0;
        break;
    case 'F':
        vt->y = vt_clamp((int)vt->y - n, vt->rows);
        vt->x = 0;
        break;
    case 'G':
    case '`':
        vt->x = vt_clamp(n - 1, vt->cols);
        break;
    case 'd':
        vt->y = vt_clamp(n - 1, vt->rows);
        break;
    case 'H':
    case 'f':
        vt->y = vt_clamp(vt_param(vt, 0, 1) - 1, vt->rows);
        vt->x = vt_clamp(vt_param(vt, 1, 1) - 1, vt->cols);
        break;
    case 'J':
        switch (vt_param(vt, 0, 0)) {
        case 0: vt_fill(vt, pos, vt->cols * vt->rows); break;
        case 1: vt_fill(vt, 0, pos + 1); break;
        case 2:
        case 3: vt_fill(vt, 0, vt->cols * vt->rows); break;
        default: break;
        }
        break;
    case 'K': {
        const size_t line = vt->y * vt->cols;
        switch (vt_param(vt, 0, 0)) {
        case 0: vt_fill(vt, pos, line + vt->cols); break;
        case 1: vt_fill(vt, line, pos + 1); break;
        case 2: vt_fill(vt, line, line + vt->cols); break;
        default: break;
        }
        break;
    }
    case 'X': {
        const size_t end = pos + (size_t)n;
        const size_t line_end = (vt->y + 1) * vt->cols;
        vt_fill(vt, pos, end < line_end ? end : line_end);
        break;
    }
    case '@':
        vt_shift_line(vt, (size_t)n, true);
        break;
    case 'P':
        vt_shift_line(vt, (size_t)n, false);
        break;
    case 'L':
        vt_scroll_down(vt, vt->y, vt->rows, (size_t)n);
        vt->x = 0;
        break;
    case 'M':
        vt_scroll_up(vt, vt->y, vt->rows, (size_t)n);
        vt->x = 0;
        break;
    case 'S':
        vt_scroll_up(vt, 0, vt->rows, (size_t)n);
        break;
    case 'T':
        vt_scroll_down(vt, 0, vt->rows, (size_t)n);
        break;
    case 'm':
        vt_sgr(vt);
        break;
    case 's':
        vt_save_cursor(vt);
        break;
    case 'u':
        vt_restore_cursor(vt);
        break;
    case 'h':
    case 'l':
        if (vt->private_mode) {
            for (size_t i = 0; i < vt->param_count; i++) {
                if (vt->params[i] == 25)
                    vt->cursor_visible = c == 'h';
            }
        }
        break;
    case 'n':
        if (vt_param(vt, 0, 0) == 5) {
            vt_respond(vt, "\x1b[0n");
        }
        else if (vt_param(vt, 0, 0) == 6) {
            vt_respond(vt, "\x1b[");
            vt_respond_number(vt, vt->y + 1);
            vt_respond(vt, ";");
            vt_respond_number(vt, vt->x + 1);
            vt_respond(vt, "R");
        }
        break;
    case 'c':
        if (!vt->private_mode)
            vt_respond(vt, "\x1b[?1;2c");
        break;
    default:
        break;
    }
}

static void vt_esc(vt_t* vt, const uint8_t c)
{
    vt->state = VT_NORMAL;
    switch (c) {
    case '[':
        vt->state = VT_CSI;
        vt->private_mode = false;
        vt->param_count = 0;
        memset(vt->params, 0, sizeof(vt->params));
        break;
    case ']':
        vt->state = VT_OSC;
        break;
    case '(':
    case ')':
    case '*':
    case '+':
        vt->state = VT_CHARSET;
        break;
    case '7':
        vt_save_cursor(vt);
        break;
    case '8':
        vt_restore_cursor(vt);
        break;
    case 'D':
        vt->wrap_pending = false;
        vt_linefeed(vt);
        break;
    case 'E':
        vt->wrap_pending = false;
        vt->x = 0;
        vt_linefeed(vt);
        break;
    case 'M':
        vt->wrap_pending = false;
        vt_reverse_index(vt);
        break;
    case 'c':
        vt_reset(vt);
        break;
    default:
        break;
    }
}

static void vt_csi_byte(vt_t* vt, const uint8_t c)
{
    if (c >= '0' && c <= '9') {
        if (vt->param_count == 0)
            vt->param_count = 1;
        int* p = &vt->params[vt->param_count - 1];
        if (*p < 10000)
            *p = *p * 10 + (c - '0');
    }
    else if (c == ';') {
        if (vt->param_count == 0)
            vt->param_count = 1;
        if (vt->param_count < VT_MAX_PARAMS)
            vt->param_count++;
    }
    else if (c == '?' || c == '>' || c == '=') {
        vt->private_mode = true;
    }
    else if (c < 0x20) {
        vt_control(vt, c);
    }
    else if (c >= 0x40 && c <= 0x7E) {
        vt->state = VT_NORMAL;
        vt_csi(vt, c);
    }
}

void vt_putc(vt_t* vt, const uint8_t c)
{
    switch (vt->state) {
    case VT_ESC:
        vt_esc(vt, c);
        return;
    case VT_CSI:
        if (c == 0x1B)
            vt->state = VT_ESC;
        else
            vt_csi_byte(vt, c);
        return;
    case VT_OSC:
        if (c == 0x07)
            vt->state = VT_NORMAL;
        else if (c == 0x1B)
            vt->state = VT_OSC_ESC;
        return;
    case VT_OSC_ESC:
    case VT_CHARSET:
        vt->state = VT_NORMAL;
        return;
    default:
        break;
    }

    if (c < 0x20 || c == 0x7F) {
        vt_control(vt, c);
        return;
    }
    if (c >= 0x80) {
        if ((c & 0xC0) == 0x80)
            return;
        vt_put_char(vt, '?');
        return;
    }
    vt_put_char(vt, c);
}
