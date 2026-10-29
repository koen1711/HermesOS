#ifndef TERMINAL_VT_H
#define TERMINAL_VT_H

#include <os/stdint.h>
#include <os/stddef.h>

#define VT_ATTR_BOLD 0x01
#define VT_ATTR_UNDERLINE 0x02
#define VT_ATTR_REVERSE 0x04

#define VT_MAX_PARAMS 16
#define VT_RESPONSE_MAX 32

typedef struct {
    uint8_t ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
} vt_cell;

typedef struct {
    vt_cell* cells;
    uint8_t* dirty;
    size_t cols;
    size_t rows;
    size_t x;
    size_t y;
    size_t saved_x;
    size_t saved_y;
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
    uint8_t saved_fg;
    uint8_t saved_bg;
    uint8_t saved_attr;
    bool wrap_pending;
    bool cursor_visible;
    uint8_t state;
    bool private_mode;
    int params[VT_MAX_PARAMS];
    size_t param_count;
    char response[VT_RESPONSE_MAX];
    size_t response_len;
} vt_t;

int vt_init(vt_t* vt, size_t cols, size_t rows);
int vt_resize(vt_t* vt, size_t cols, size_t rows);
void vt_putc(vt_t* vt, uint8_t c);
void vt_mark_dirty(vt_t* vt);

#endif //TERMINAL_VT_H
